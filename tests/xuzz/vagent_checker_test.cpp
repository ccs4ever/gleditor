#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "common/xanadu/vagent/checker.hpp"
#include "common/xanadu/vagent/parser.hpp"

namespace {
using namespace xanadu::vagent;

std::vector<Diagnostic> diagnostics(std::string_view source) {
  auto program = Parser(source).parseProgram();
  if (!program) {
    ADD_FAILURE() << "checker test must first parse: "
                  << formatDiagnostic(program.error());
    return {program.error()};
  }
  auto result = check(*program);
  return result ? std::vector<Diagnostic>{} : std::move(result.error());
}

std::string describe(const std::vector<Diagnostic> &errors) {
  std::string result;
  for (const auto &error : errors) result += formatDiagnostic(error) + '\n';
  return result;
}

void rejects(std::string_view source, DiagnosticCode code) {
  const auto errors = diagnostics(source);
  EXPECT_FALSE(errors.empty());
  EXPECT_TRUE(std::ranges::any_of(errors, [&](const Diagnostic &error) {
    return error.code == code;
  })) << describe(errors);
}

TEST(VagentCheckerTest, AllTwentyThreeDesignExamplesCheck) {
  for (int index = 1; index <= 23; ++index) {
    std::ostringstream path;
    path << "tests/samples/vagent/mini_" << std::setw(2) << std::setfill('0')
         << index << ".vagent";
    SCOPED_TRACE(path.str());
    std::ifstream input(path.str());
    ASSERT_TRUE(input.is_open());
    const std::string source{std::istreambuf_iterator<char>(input), {}};
    const auto errors = diagnostics(source);
    EXPECT_TRUE(errors.empty()) << describe(errors);
  }
}

TEST(VagentCheckerTest, AllowsOptionalAssignmentsAndReads) {
  const auto errors = diagnostics(R"(
    event Tick
    goal Work() perform
    agent Worker {
      var task: Opt<Intention> := none
      on Tick {
        task := adopt Work()
        if task then drop task
        task := none
      }
      plan work for Work { yield }
    }
  )");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, PreservesQueryBindingsOnlyInTheirSuccessScope) {
  const auto valid = diagnostics(R"(
    predicate position(value: Int)
    event Tick
    goal Work() perform
    agent Worker {
      var result: Int := 0
      on Tick { if believes position(?x) then result := x }
      plan work for Work where believes position(?x) { result := x }
    }
  )");
  EXPECT_TRUE(valid.empty()) << describe(valid);
  rejects(R"(
    predicate position(value: Int)
    event Tick
    agent Worker {
      var result: Int := 0
      on Tick {
        if believes position(?x) then result := x
        result := x
      }
    }
  )",
          DiagnosticCode::UnknownName);
}

TEST(VagentCheckerTest, AwaitBindingsAreAvailableAfterResumption) {
  const auto errors = diagnostics(R"(
    predicate position(value: Int)
    goal Work() perform
    agent Worker {
      var result: Int := 0
      plan work for Work {
        await position(?x) within 1s
        result := x
      }
    }
  )");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, RejectsBareBoolConditionsAndBooleanSearchOperands) {
  rejects(
      R"(event Tick agent A { var enabled: Bool := true on Tick [enabled] {} })",
      DiagnosticCode::TypeMismatch);
  rejects(R"(event Tick agent A { on Tick { true & () } })",
          DiagnosticCode::TypeMismatch);
  rejects(R"(event Tick agent A { on Tick { not false } })",
          DiagnosticCode::TypeMismatch);
}

TEST(VagentCheckerTest, RejectsTransitiveImpureConditions) {
  rejects(R"(
    event Tick
    agent A {
      var count: Int := 0
      def mutate(): Int { count := count + 1; count }
      def indirectly(): Int { mutate() }
      on Tick [indirectly() > 0] {}
    }
  )",
          DiagnosticCode::ImpureCondition);
}

TEST(VagentCheckerTest, RejectsActionsAndReversibleWritesInConditions) {
  rejects(R"(event Tick event Output agent A { on Tick [emit Output{}] {} })",
          DiagnosticCode::ImpureCondition);
  rejects(R"(event Tick agent A { var n: Int := 0 on Tick [n <- 1] {} })",
          DiagnosticCode::ImpureCondition);
}

TEST(VagentCheckerTest, ReadOnlyAnswersAreTransitivelyChecked) {
  rejects(R"(
    question total(): Int
    agent A {
      var count: Int := 0
      def mutate(): Int { count := 1; count }
      answer total(): Int { mutate() }
    }
  )",
          DiagnosticCode::ImpureCondition);
}

TEST(VagentCheckerTest, FieldInitializersMustBePure) {
  rejects(R"(
    event Tick
    agent A {
      var count: Int := mutate()
      def mutate(): Int { count := 1; count }
    }
  )",
          DiagnosticCode::ImpureCondition);
}

TEST(VagentCheckerTest, IrreversibleAssignmentIsNotAnOperandOfSearch) {
  rejects(
      R"(event Tick agent A { var count: Int := 0 on Tick { (count := 1) & () } })",
      DiagnosticCode::InvalidPosition);
  const auto errors = diagnostics(
      R"(event Tick agent A { var count: Int := 0 on Tick { (count <- 1) & () } })");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, EffectsRequireNativeImplementationAndPositiveDeadline) {
  rejects(R"(capacity Device { effect def run() })",
          DiagnosticCode::MissingDeadline);
  rejects(R"(capacity Device { effect def run() within 0s })",
          DiagnosticCode::MissingDeadline);
  rejects(R"(
    capacity Device { effect def run() within 1s }
    skill Bad implements Device { effect def run() within 1s {} }
  )",
          DiagnosticCode::InvalidDeclaration);
  rejects(R"(capacity Device { def run() } native skill Bad implements Device)",
          DiagnosticCode::InvalidDeclaration);
}

TEST(VagentCheckerTest, EffectsCannotHideInsideExpressionsOrMethods) {
  rejects(R"(
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    goal Work() perform
    agent A { uses Device install Host()
      plan work for Work { let value := 1 + read() }
    }
  )",
          DiagnosticCode::InvalidPosition);
  rejects(R"(
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    agent A { uses Device install Host()
      def helper(): Int { read() }
    }
  )",
          DiagnosticCode::InvalidPosition);
}

TEST(VagentCheckerTest, HandlerMayStageAnEffectButCannotReadItsResult) {
  const auto valid = diagnostics(R"(
    event Tick
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    agent A { uses Device install Host() on Tick { read() } }
  )");
  EXPECT_TRUE(valid.empty()) << describe(valid);
  rejects(R"(
    event Tick
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    agent A { uses Device install Host() on Tick { let result := read() } }
  )",
          DiagnosticCode::InvalidPosition);
}

TEST(VagentCheckerTest, AskAndYieldArePlanSuspensionStatements) {
  rejects(
      R"(question count(): Int event Tick agent A(peer: Agent) { on Tick { let n := ask peer.count() within 1s } })",
      DiagnosticCode::InvalidPosition);
  rejects(R"(event Tick agent A { on Tick { yield } })",
          DiagnosticCode::InvalidPosition);
}

TEST(VagentCheckerTest, BeliefLoopsCannotSuspend) {
  rejects(R"(
    predicate item(value: Int)
    goal Work() perform
    agent A { plan work for Work { for believes item(?x) do yield } }
  )",
          DiagnosticCode::InvalidPosition);
}

TEST(VagentCheckerTest, WaitDeadlineOperandsMustBeDurations) {
  rejects(
      R"(predicate ready() goal Work() perform agent A { plan work for Work { await ready() within "soon" } })",
      DiagnosticCode::TypeMismatch);
  const auto errors = diagnostics(
      R"(predicate ready() goal Work() perform agent A(wait: Duration) { plan work for Work { await ready() within wait } })");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, ReturnTypesAndPositionsAreChecked) {
  rejects(R"(agent A { def count(): Int { "wrong" } })",
          DiagnosticCode::TypeMismatch);
  rejects(R"(agent A { def count(): Int { return "wrong" } })",
          DiagnosticCode::TypeMismatch);
  rejects(R"(event Tick agent A { on Tick { return 1 } })",
          DiagnosticCode::InvalidPosition);
}

TEST(VagentCheckerTest,
     CallsCheckArityArgumentTypesAndCapacityImplementations) {
  rejects(
      R"(event Tick agent A { def count(n: Int): Int { n } on Tick { count("wrong") } })",
      DiagnosticCode::TypeMismatch);
  rejects(
      R"(event Tick agent A { def count(n: Int): Int { n } on Tick { count() } })",
      DiagnosticCode::TypeMismatch);
  rejects(
      R"(capacity Scale { def adjust(n: Int): Int } skill Broken implements Scale {})",
      DiagnosticCode::InvalidDeclaration);
  rejects(
      R"(capacity Scale { def adjust(n: Int): Int } skill Broken implements Scale { def adjust(n: Str): Int { 1 } })",
      DiagnosticCode::TypeMismatch);
}

TEST(VagentCheckerTest, TypeNamesAndNameConventionsAreChecked) {
  rejects(R"(agent lower {})", DiagnosticCode::InvalidIdentifier);
  rejects(R"(agent A { var Count: Int := 0 })",
          DiagnosticCode::InvalidIdentifier);
  rejects(R"(agent A { var value: Missing := 0 })",
          DiagnosticCode::UnknownName);
  rejects(R"(agent A { var value: Opt<Int, Str> := none })",
          DiagnosticCode::TypeMismatch);
}

TEST(VagentCheckerTest, RejectsDuplicateNamesAndNonLeadingPredicateKeys) {
  rejects(R"(event Tick event Tick)", DiagnosticCode::DuplicateName);
  rejects(R"(agent A { var count: Int := 0 var count: Int := 1 })",
          DiagnosticCode::DuplicateName);
  rejects(R"(predicate item(value: Int, key id: Str))",
          DiagnosticCode::InvalidDeclaration);
}

TEST(VagentCheckerTest, BeliefAndQuestionSignaturesAreNotUntypedEscapeHatches) {
  rejects(
      R"(predicate item(value: Int) event Tick agent A { on Tick { believe item("wrong") } })",
      DiagnosticCode::TypeMismatch);
  rejects(
      R"(predicate item(value: Int) event Tick agent A { on Tick { believe item(?x) } })",
      DiagnosticCode::UnknownName);
  rejects(
      R"(question count(): Int agent A { answer count(): Str { "wrong" } })",
      DiagnosticCode::TypeMismatch);
  rejects(R"(agent A { answer absent(): Int { 0 } })",
          DiagnosticCode::UnknownName);
}

TEST(VagentCheckerTest, EventsHaveImplicitTypedMetadataAndInheritance) {
  const auto errors = diagnostics(R"(
    event Base { value: Int }
    event Detail extends Base { tag: Str }
    agent A {
      on Base as e { let value := e.value; let source := e.source; let time := e.time }
      on Detail as e { let value := e.value; let tag := e.tag }
    }
  )");
  EXPECT_TRUE(errors.empty()) << describe(errors);
  rejects(R"(event Bad { source: Agent })", DiagnosticCode::InvalidDeclaration);
  rejects(R"(event Base { value: Int } event Bad extends Base { value: Int })",
          DiagnosticCode::DuplicateName);
  rejects(R"(event A extends B event B extends A)",
          DiagnosticCode::InvalidDeclaration);
}

TEST(VagentCheckerTest, RecordFieldsAreCheckedByNameAndType) {
  rejects(
      R"(record Point { x: Int } event Tick agent A { on Tick { let p := Point{} } })",
      DiagnosticCode::TypeMismatch);
  rejects(
      R"(record Point { x: Int } event Tick agent A { on Tick { let p := Point{x: "wrong"} } })",
      DiagnosticCode::TypeMismatch);
  rejects(
      R"(record Point { x: Int } event Tick agent A { on Tick { let p := Point{x: 1, x: 2} } })",
      DiagnosticCode::DuplicateName);
}

TEST(VagentCheckerTest, UnknownNamesAndUnavailableModulesAreRefused) {
  rejects(R"(event Tick agent A { on Tick { missing() } })",
          DiagnosticCode::UnknownName);
  rejects(R"(import other:module agent A {})",
          DiagnosticCode::InvalidDeclaration);
  rejects(R"(agent A { uses Missing })", DiagnosticCode::UnknownName);
}

TEST(VagentCheckerTest, NumericTypesDoNotCoerceAndIntMinimumIsContextual) {
  rejects(R"(agent A { var count: Int := 1.0 })", DiagnosticCode::TypeMismatch);
  rejects(R"(agent A { var count: Int := 9223372036854775808 })",
          DiagnosticCode::TypeMismatch);
  const auto errors =
      diagnostics(R"(agent A { var count: Int := -9223372036854775808 })");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, GoalKindsRequireTheirDeclaredConditions) {
  rejects(R"(goal Ready() achieve agent A { on Initialize { adopt Ready() } })",
          DiagnosticCode::InvalidDeclaration);
  rejects(
      R"(goal Ready() maintain agent A { on Initialize { adopt Ready() } })",
      DiagnosticCode::InvalidDeclaration);
  rejects(R"(goal Work() perform { succeed when 1 == 1 })",
          DiagnosticCode::InvalidDeclaration);
}

TEST(VagentCheckerTest, NominalRecordsAndBooleansHaveNoInventedOrdering) {
  rejects(R"(record Point { x: Int } event Tick agent A {
    on Tick { if Point{x: 1} < Point{x: 2} then () }
  })",
          DiagnosticCode::TypeMismatch);
  rejects(R"(event Tick agent A { on Tick { if true < false then () } })",
          DiagnosticCode::TypeMismatch);
  const auto errors = diagnostics(R"(event Tick agent A {
    on Tick { if "a" < "b" then () }
  })");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, BuiltinTypeNamesCannotBeShadowed) {
  rejects(R"(record Int { value: Str })", DiagnosticCode::DuplicateName);
  rejects(R"(agent Agent {})", DiagnosticCode::DuplicateName);
  rejects(R"(record Opt { value: Int })", DiagnosticCode::DuplicateName);
}

TEST(VagentCheckerTest, InheritedCapacitySignaturesMustAgree) {
  rejects(R"(
    capacity First { def value(n: Int): Int }
    capacity Second { def value(n: Str): Int }
    capacity Both extends First, Second {}
  )",
          DiagnosticCode::TypeMismatch);
  rejects(R"(
    capacity First { effect def value() within 1s }
    capacity Second { effect def value() within 2s }
    capacity Both extends First, Second {}
  )",
          DiagnosticCode::TypeMismatch);
  rejects(R"(
    capacity First { effect def value() within 1s }
    capacity Second { def value() }
    capacity Both extends First, Second {}
  )",
          DiagnosticCode::TypeMismatch);
  rejects(R"(
    capacity First { effect preempts def value() within 1s }
    capacity Second { effect def value() within 1s }
    capacity Both extends First, Second {}
  )",
          DiagnosticCode::TypeMismatch);
  rejects(R"(
    capacity First { def value(n: Int): Int }
    capacity Second { def value(n: Str): Int }
    skill Both implements First, Second { def value(n: Str): Int { 0 } }
  )",
          DiagnosticCode::TypeMismatch);
  const auto errors = diagnostics(R"(
    capacity First { effect def value() within 1s }
    capacity Second { effect def value() within 1_000ms }
    capacity Both extends First, Second {}
    native skill Host implements Both
  )");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, NestedInitializerExpressionsCannotSuspend) {
  rejects(R"(
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    goal Work() perform
    agent A { uses Device install Host()
      plan work for Work {
        let value := if 1 == 1 then read() else read()
      }
    }
  )",
          DiagnosticCode::InvalidPosition);
  const auto errors = diagnostics(R"(
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    goal Work() perform
    agent A { uses Device install Host()
      plan work for Work { if 1 == 1 then read() else read() }
    }
  )");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, ReversibleAssignmentsCannotWrapSuspension) {
  rejects(R"(
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    goal Work() perform
    agent A { uses Device install Host() var result: Int := 0
      plan work for Work { result <- read() }
    }
  )",
          DiagnosticCode::InvalidPosition);
  rejects(R"(
    question count(): Int
    goal Work() perform
    agent A(peer: Agent) { var result: Int := 0
      plan work for Work { result <- ask peer.count() within 1s }
    }
  )",
          DiagnosticCode::InvalidPosition);
}

TEST(VagentCheckerTest, NegationDoesNotExportFailedQueryBindings) {
  rejects(R"(
    predicate item(value: Int)
    event Tick
    agent A { var result: Int := 0
      on Tick { if not believes item(?x) then result := x }
    }
  )",
          DiagnosticCode::UnknownName);
}

TEST(VagentCheckerTest, OnlySuccessfulAlternativesConstrainDefiniteBindings) {
  for (const auto condition :
       {"(believes item(?x) | fail)", "(fail | believes item(?x))"}) {
    const auto errors =
        diagnostics(std::string("predicate item(value: Int) event Tick agent A "
                                "{ var result: Int := 0 on Tick { if ") +
                    condition + " then result := x } }");
    EXPECT_TRUE(errors.empty()) << describe(errors);
  }
}

TEST(VagentCheckerTest, PureHelpersMayReadTopLevelIntentionContents) {
  const auto errors = diagnostics(R"(
    event Tick
    agent A {
      def how_many(): Int { *intentions() }
      on Tick [how_many() > 0] {}
    }
  )");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, DerivedCapacityImplementationProvidesPureBaseMethod) {
  const auto errors = diagnostics(R"(
    capacity Base { def sample(): Int }
    capacity Derived extends Base {}
    skill Implementation implements Derived { def sample(): Int { 1 } }
    event Tick
    agent A { uses Base install Implementation()
      on Tick [sample() > 0] {}
    }
  )");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest,
     WildcardFactsMustNotBeAssertedAndBindingsHaveValueNames) {
  rejects(R"(predicate item(value: Int) event Tick agent A {
    on Tick { believe item(?_) }
  })",
          DiagnosticCode::InvalidPosition);
  rejects(R"(predicate item(value: Int) event Tick agent A {
    on Tick { tell parent item(?_) }
  })",
          DiagnosticCode::InvalidPosition);
  rejects(R"(predicate item(value: Int) event Tick agent A {
    on Tick [believes item(?Value)] {}
  })",
          DiagnosticCode::InvalidIdentifier);
  const auto errors =
      diagnostics(R"(predicate item(value: Int) event Tick agent A {
    on Tick { forget item(?_) }
  })");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest,
     SignedMinimumMagnitudeIsParsedRatherThanSpellingMatched) {
  for (const auto literal : {"-0x08000000000000000", "-09223372036854775808",
                             "-0X8000000000000000"}) {
    const auto errors = diagnostics(
        std::string("agent A { var minimum: Int := ") + literal + " }");
    EXPECT_TRUE(errors.empty()) << describe(errors);
  }
  rejects(R"(agent A { var minimum: Int := 0x08000000000000000 })",
          DiagnosticCode::TypeMismatch);
}

TEST(VagentCheckerTest, InheritanceDiamondsVisitCompletedSubgraphsOnce) {
  std::string source = "capacity C0 {} capacity C1 extends C0 {} ";
  for (int i = 2; i <= 24; ++i)
    source += "capacity C" + std::to_string(i) + " extends C" +
              std::to_string(i - 1) + ", C" + std::to_string(i - 2) + " {} ";
  const auto errors = diagnostics(source);
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, EffectRecoveryHasTypedCauseAndValueFallback) {
  const auto errors = diagnostics(R"(
    event Problem { cause: Str }
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    goal Work() perform
    agent A { uses Device install Host() var result: Int := 0
      plan work for Work {
        result := read() else 0
        let next := read() else { emit Problem{cause: cause}; 0 }
        result := next
      }
    }
  )");
  EXPECT_TRUE(errors.empty()) << describe(errors);
  rejects(R"(
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    goal Work() perform
    agent A { uses Device install Host()
      plan work for Work { let value := read() else "wrong" }
    }
  )",
          DiagnosticCode::TypeMismatch);
}

TEST(VagentCheckerTest, DiscardedEffectResultAllowsRecoveryActionsAndWaits) {
  const auto errors = diagnostics(R"(
    predicate ready()
    event Problem { cause: Str }
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    goal Work() perform
    agent A { uses Device install Host()
      plan work for Work {
        read() else { emit Problem{cause: cause}; await ready() within 1s }
      }
    }
  )");
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, RecoveryDoesNotTurnOrdinaryCallsIntoEffectStatements) {
  rejects(R"(
    goal Work() perform
    agent A { def value(): Int { 1 }
      plan work for Work { let result := value() else 0 }
    }
  )",
          DiagnosticCode::InvalidPosition);
  rejects(R"(
    capacity Device { def read(): Int }
    skill Host implements Device { def read(): Int { 1 } }
    goal Work() perform
    agent A { uses Device install Host()
      plan work for Work { read() else 0 }
    }
  )",
          DiagnosticCode::InvalidPosition);
}

TEST(VagentCheckerTest, HandlerEffectsHaveNoSynchronousRecoveryBranch) {
  rejects(R"(
    event Tick
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    agent A { uses Device install Host()
      on Tick { read() else 0 }
    }
  )",
          DiagnosticCode::InvalidPosition);
}

TEST(VagentCheckerTest, EffectRecoveryCauseDoesNotEscapeItsBranch) {
  rejects(R"(
    capacity Device { effect def read(): Int within 1s }
    native skill Host implements Device
    goal Work() perform
    agent A { uses Device install Host()
      plan work for Work { read() else 0; Logging.info(cause) }
    }
  )",
          DiagnosticCode::UnknownName);
}

TEST(VagentCheckerTest, DeepFlatCapacityChainsUsePreparedIterativeInterfaces) {
  constexpr int depth = 8000;
  std::string source;
  for (int i = depth; i > 0; --i)
    source += "capacity C" + std::to_string(i) + " extends C" +
              std::to_string(i - 1) + " {} ";
  source +=
      "capacity C0 { def value(): Int } skill Implementation implements C" +
      std::to_string(depth) +
      " { def value(): Int { 1 } } "
      "event Tick agent A { uses C0 install Implementation() "
      "on Tick [value() > 0] {} }";
  const auto errors = diagnostics(source);
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, DeepFlatEventChainsRetainTypedInheritedFields) {
  constexpr int depth = 2000;
  std::string source;
  for (int i = depth; i > 0; --i)
    source += "event E" + std::to_string(i) + " extends E" +
              std::to_string(i - 1) + " {} ";
  source += "event E0 { value: Int } agent A { on E" + std::to_string(depth) +
            " as e { let value := e.value } }";
  const auto errors = diagnostics(source);
  EXPECT_TRUE(errors.empty()) << describe(errors);
}

TEST(VagentCheckerTest, DeepInheritanceCycleIsDiagnosedWithoutNativeRecursion) {
  constexpr int depth = 8000;
  std::string source;
  for (int i = depth; i > 0; --i)
    source += "capacity C" + std::to_string(i) + " extends C" +
              std::to_string(i - 1) + " {} ";
  source += "capacity C0 extends C" + std::to_string(depth) + " {}";
  rejects(source, DiagnosticCode::InvalidDeclaration);
}

TEST(VagentCheckerTest, InheritanceWorkLimitsRefuseWithoutPartialSuccess) {
  auto program =
      Parser(
          "capacity Base { def value(): Int } capacity Derived extends Base {}")
          .parseProgram();
  ASSERT_TRUE(program.has_value());
  auto limited = check(*program, CheckLimits{.maxInheritanceWork = 1});
  ASSERT_FALSE(limited.has_value());
  EXPECT_TRUE(std::ranges::any_of(limited.error(), [](const Diagnostic &error) {
    return error.detail.find("CheckLimits::maxInheritanceWork") !=
           std::string::npos;
  }));
  auto ample = check(*program, CheckLimits{.maxInheritanceWork = 100});
  EXPECT_TRUE(ample.has_value());
}

TEST(VagentCheckerTest, FlattenedInterfaceWorkIsAccountedNotJustGraphDepth) {
  std::string source = "capacity C0 { def m0(): Int } ";
  for (int i = 1; i <= 100; ++i)
    source += "capacity C" + std::to_string(i) + " extends C" +
              std::to_string(i - 1) + " { def m" + std::to_string(i) +
              "(): Int } ";
  auto program = Parser(source).parseProgram();
  ASSERT_TRUE(program.has_value());
  auto limited = check(*program, CheckLimits{.maxInheritanceWork = 1000});
  ASSERT_FALSE(limited.has_value());
  EXPECT_TRUE(std::ranges::any_of(limited.error(), [](const Diagnostic &error) {
    return error.detail.find("CheckLimits::maxInheritanceWork") !=
           std::string::npos;
  }));
  auto ample = check(*program, CheckLimits{.maxInheritanceWork = 100'000});
  EXPECT_TRUE(ample.has_value()) << (ample ? "" : describe(ample.error()));
}

TEST(VagentCheckerTest, PublicAstExpressionDepthIsCheckedBeforePurity) {
  auto program = Parser("agent A { def value(): Int { 1 } }").parseProgram();
  ASSERT_TRUE(program.has_value());
  auto &body = program->declarations.front().members.front().body;
  for (int i = 0; i < 300; ++i) {
    auto unary  = std::make_unique<Expr>();
    unary->kind = ExprKind::Unary;
    unary->text = "-";
    unary->children.push_back(std::move(body));
    body = std::move(unary);
  }
  auto limited = check(*program);
  ASSERT_FALSE(limited.has_value());
  EXPECT_NE(describe(limited.error()).find("maxExpressionDepth"),
            std::string::npos);
  auto ample = check(*program, CheckLimits{.maxExpressionDepth = 400});
  EXPECT_TRUE(ample.has_value()) << (ample ? "" : describe(ample.error()));
}

TEST(VagentCheckerTest, PublicAstExpressionNodeBudgetCountsWideTrees) {
  auto program =
      Parser("agent A { def values(): [Int] { [1] } }").parseProgram();
  ASSERT_TRUE(program.has_value());
  auto &body = program->declarations.front().members.front().body;
  body       = std::make_unique<Expr>();
  body->kind = ExprKind::List;
  for (int i = 0; i < 300; ++i) {
    auto value  = std::make_unique<Expr>();
    value->kind = ExprKind::Integer;
    value->text = "1";
    body->children.push_back(std::move(value));
  }
  auto limited = check(*program, CheckLimits{.maxExpressionNodes = 100});
  ASSERT_FALSE(limited.has_value());
  EXPECT_NE(describe(limited.error()).find("maxExpressionNodes"),
            std::string::npos);
  EXPECT_TRUE(check(*program).has_value());
}

TEST(VagentCheckerTest, PreflightVisitsEveryExpressionMetadataEdge) {
  for (const auto member : {&Expr::scope, &Expr::recipients, &Expr::deadline,
                            &Expr::guard, &Expr::recovery}) {
    auto program = Parser("agent A { def value(): Int { 1 } }").parseProgram();
    ASSERT_TRUE(program.has_value());
    auto &body   = program->declarations.front().members.front().body;
    Expr *cursor = body.get();
    for (int i = 0; i < 300; ++i) {
      cursor->*member = std::make_unique<Expr>();
      cursor          = (cursor->*member).get();
    }
    auto limited = check(*program);
    ASSERT_FALSE(limited.has_value());
    EXPECT_NE(describe(limited.error()).find("maxExpressionDepth"),
              std::string::npos);
  }
}

TEST(VagentCheckerTest, PublicAstDeclarationAndTypeNestingHaveLimits) {
  Program program;
  program.declarations.emplace_back();
  Decl *cursor = &program.declarations.back();
  for (int i = 0; i < 300; ++i) {
    cursor->members.emplace_back();
    cursor = &cursor->members.back();
  }
  auto declarations = check(program);
  ASSERT_FALSE(declarations.has_value());
  EXPECT_NE(describe(declarations.error()).find("maxDeclarationDepth"),
            std::string::npos);

  auto typed = Parser("agent A { def value(): Int { 1 } }").parseProgram();
  ASSERT_TRUE(typed.has_value());
  Type *type = &*typed->declarations.front().members.front().result;
  for (int i = 0; i < 300; ++i) {
    type->name = "Opt";
    type->arguments.emplace_back();
    type = &type->arguments.back();
  }
  type->name = "Int";
  auto types = check(*typed);
  ASSERT_FALSE(types.has_value());
  EXPECT_NE(describe(types.error()).find("maxTypeDepth"), std::string::npos);
}

TEST(VagentCheckerTest, PublicAstTotalNodeBudgetAndEventShapeAreValidated) {
  Program program;
  program.declarations.resize(300);
  auto total = check(program, CheckLimits{.maxAstNodes = 100});
  ASSERT_FALSE(total.has_value());
  EXPECT_NE(describe(total.error()).find("maxAstNodes"), std::string::npos);
  program.declarations.resize(1);
  auto &event = program.declarations.front();
  event.kind  = DeclKind::Event;
  event.name  = "Derived";
  event.names = {"Base", "Other"};
  auto shape  = check(program);
  ASSERT_FALSE(shape.has_value());
  EXPECT_NE(describe(shape.error()).find("only one base event"),
            std::string::npos);
}

TEST(VagentCheckerTest, PurityWorkBoundsRepeatedReverseCallChainPasses) {
  std::string source = "event Tick agent A { var count: Int := 0 ";
  for (int i = 0; i < 80; ++i)
    source += "def m" + std::to_string(i) + "(): Int { m" +
              std::to_string(i + 1) + "() } ";
  source += "def m80(): Int { count := count + 1; count } ";
  auto program = Parser(source + "}").parseProgram();
  ASSERT_TRUE(program.has_value());
  auto limited = check(*program, CheckLimits{.maxPurityWork = 2000});
  ASSERT_FALSE(limited.has_value());
  EXPECT_NE(describe(limited.error()).find("CheckLimits::maxPurityWork"),
            std::string::npos);
  auto ample = check(*program, CheckLimits{.maxPurityWork = 100'000});
  EXPECT_TRUE(ample.has_value()) << (ample ? "" : describe(ample.error()));

  auto guarded = Parser(source + "on Tick [m0() > 0] {} }").parseProgram();
  ASSERT_TRUE(guarded.has_value());
  auto impure = check(*guarded, CheckLimits{.maxPurityWork = 100'000});
  ASSERT_FALSE(impure.has_value());
  EXPECT_TRUE(std::ranges::any_of(impure.error(), [](const Diagnostic &error) {
    return error.code == DiagnosticCode::ImpureCondition;
  })) << describe(impure.error());
}

TEST(VagentCheckerTest, LargeFlatOwnerResolvesLastMethodWithoutRepeatedScans) {
  std::string source = "agent A { ";
  for (int i = 0; i < 8000; ++i)
    source += "def m" + std::to_string(i) + "(): Int { last() } ";
  source += "def last(): Int { 1 } }";
  auto program = Parser(source).parseProgram();
  ASSERT_TRUE(program.has_value());
  auto result = check(*program);
  EXPECT_TRUE(result.has_value()) << (result ? "" : describe(result.error()));
}

TEST(VagentCheckerTest, ResolutionWorkRefusalCanBeRetriedWithLargerBudget) {
  auto program = Parser(R"(
    capacity Reader { def read(): Int }
    skill Host implements Reader { def read(): Int { 1 } }
    event Tick
    agent A { uses Reader install Host()
      var count: Int := 0
      def value(): Int { read() }
      on Tick [value() > 0] { count := value() }
    }
  )")
                     .parseProgram();
  ASSERT_TRUE(program.has_value());
  auto limited = check(*program, CheckLimits{.maxResolutionWork = 20});
  ASSERT_FALSE(limited.has_value());
  EXPECT_NE(describe(limited.error()).find("CheckLimits::maxResolutionWork"),
            std::string::npos);
  auto ample = check(*program, CheckLimits{.maxResolutionWork = 10'000});
  EXPECT_TRUE(ample.has_value()) << (ample ? "" : describe(ample.error()));
}

TEST(VagentCheckerTest, RepeatedInheritedSignatureComparisonsDebitTypeWeight) {
  std::string parameters;
  for (int i = 0; i < 80; ++i) {
    if (i != 0) parameters += ", ";
    parameters += "p" + std::to_string(i) + ": Opt<[Int]>";
  }
  std::string source = "capacity C0 { def value(" + parameters +
                       "): Int } capacity Other { def value(" + parameters +
                       "): Int } ";
  for (int i = 1; i <= 60; ++i)
    source += "capacity C" + std::to_string(i) + " extends C" +
              std::to_string(i - 1) + ", Other {} ";
  auto program = Parser(source).parseProgram();
  ASSERT_TRUE(program.has_value());
  auto limited = check(*program, CheckLimits{.maxResolutionWork = 3000});
  ASSERT_FALSE(limited.has_value());
  EXPECT_NE(describe(limited.error()).find("CheckLimits::maxResolutionWork"),
            std::string::npos);
  auto ample = check(*program, CheckLimits{.maxResolutionWork = 100'000});
  EXPECT_TRUE(ample.has_value()) << (ample ? "" : describe(ample.error()));
}

TEST(VagentCheckerTest, LargeGoalPlanBindingsUseIndexedParameterNames) {
  constexpr int count = 3000;
  std::string source  = "goal Work(";
  for (int i = 0; i < count; ++i) {
    if (i != 0) source += ", ";
    source += "p" + std::to_string(i) + ": Int";
  }
  source += ") perform agent A { plan work for Work(";
  for (int i = count - 1; i >= 0; --i) {
    if (i != count - 1) source += ", ";
    source += "p" + std::to_string(i);
  }
  source += ") { let value := p2999 + p0 } }";
  auto program = Parser(source).parseProgram();
  ASSERT_TRUE(program.has_value());
  auto limited = check(*program, CheckLimits{.maxResolutionWork = 100});
  ASSERT_FALSE(limited.has_value());
  EXPECT_NE(describe(limited.error()).find("CheckLimits::maxResolutionWork"),
            std::string::npos);
  auto ample = check(*program);
  EXPECT_TRUE(ample.has_value()) << (ample ? "" : describe(ample.error()));
  rejects("goal Work() perform agent A { plan work for Work(missing) {} }",
          DiagnosticCode::UnknownName);
}

TEST(VagentCheckerTest, ManyGlobalGoalsRetainConcreteCheckedOwnerContexts) {
  std::string source = "goal G0() achieve { succeed when count > 0 } ";
  for (int i = 1; i < 4000; ++i)
    source += "goal G" + std::to_string(i) + "() perform ";
  source += "agent A { var count: Int := 1 ";
  for (int i = 0; i < 4000; ++i)
    source +=
        "plan p" + std::to_string(i) + " for G" + std::to_string(i) + " {} ";
  source += "}";
  auto program = Parser(source).parseProgram();
  ASSERT_TRUE(program.has_value());
  auto result = check(*program);
  EXPECT_TRUE(result.has_value()) << (result ? "" : describe(result.error()));
}

} // namespace
