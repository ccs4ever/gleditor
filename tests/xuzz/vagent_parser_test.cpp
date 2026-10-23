/**
 * @file vagent_parser_test.cpp
 * @brief Tests for the vagent front end: lexer, AST shape and parser
 * (design/vagent-agent-language.md §8).
 */
#include <gtest/gtest.h>

#include <string>
#include <string_view>
#include <type_traits>
#include <variant>

#include "common/xanadu/vagent/ast.hpp"
#include "common/xanadu/vagent/lexer.hpp"
#include "common/xanadu/vagent/parser.hpp"

namespace {

using namespace xanadu::vagent;

// The examples are copied verbatim from the design document.
// clang-format off
constexpr const char *goals62 = R"vagent(
goal Reach(location: Point) achieve {
  succeed when believes position(location)
}
goal KeepCharged(floor: Float) maintain {
  holds believes battery(?level) from self & level > floor
}
goal Survey(target_id: Str, location: Point) perform
)vagent";

constexpr const char *plan62 = R"vagent(
plan direct for Reach where believes battery(?level) & level > reserve_floor {
  follow([location])
}
)vagent";

constexpr const char *percept75 = R"vagent(
on Battery as b { revise battery(b.level) }
)vagent";

constexpr const char *declarations85 = R"vagent(
predicate at(key who: Agent, x: Float, y: Float)
question heading(): Float

event TargetDetected { target_id: Str, distance: Float }
event Alarm extends TargetDetected { level: Int }

capacity Navigation {
  effect def move(speed: Float, heading: Float) within 30s requires speed >= 0.0
  effect def stop_engines() within 2s
}

native skill ThrusterNav(max_speed: Float) implements Navigation

behavior ObstacleAvoidance(threshold: Int) {
  uses Navigation
  on CollisionWarning as w [w.threat > threshold] priority 1 {
    stop_engines()
    emit ObstacleEvaded{obstacle: w.obstacle_id}
  }
}

agent PatrolDrone(threshold: Int, max_speed: Float) {
  uses Navigation
  install ThrusterNav(max_speed)
  behavior ObstacleAvoidance(threshold)
  var targets_seen: Int := 0
  var bearing: Float := 0.0

  answer heading(): Float { bearing }

  on TargetDetected [occurrence.distance < 50.0] {
    targets_seen := targets_seen + 1
    emit TargetLogged{target: occurrence.target_id}
  }
  on +at(?who, ?x, ?y) [who == self] {
    emit Relocated{x: x, y: y}
  }
}
)vagent";

constexpr const char *search121 = R"vagent(
module demo:search
import std:agent

predicate pending(query_id: Str)
predicate reported(query_id: Str, partition: Int)
predicate hit(query_id: Str, text: Str)

event SearchQuery { query_id: Str, pattern: Str }
event SubTask { query_id: Str, partition: Int, pattern: Str }
event PartitionResult { query_id: Str, partition: Int, hits: [Str] }
event SearchCompleted { query_id: Str, hits: [Str] }

agent Worker {
  on SubTask as t {
    emit PartitionResult{query_id: t.query_id, partition: t.partition,
                         hits: scan_partition(t.partition, t.pattern)} for t.source
  }
  def scan_partition(index: Int, pattern: Str): [Str] {
    ["alpha-" ++ text(index), "beta-" ++ text(index)]
  }
}

agent SearchCoordinator(partitions: Int) {
  var workers: [Agent] := []

  on Initialize {
    for _ in 1 to partitions do workers := workers ++ [spawn Worker() in inner]
  }

  on SearchQuery as q [q.space == outer] {
    believe pending(q.query_id)
    for i in 0 to partitions - 1 do
      emit SubTask{query_id: q.query_id, partition: i, pattern: q.pattern} in inner for workers[i]
  }

  // A duplicate delivery of one partition fails the guard and changes nothing.
  on PartitionResult as r [r.space == inner & not believes reported(r.query_id, r.partition)] {
    believe reported(r.query_id, r.partition)
    for h in r.hits do believe hit(r.query_id, h)
    if *findall(?p, reported(r.query_id, ?p)) == partitions then {
      emit SearchCompleted{query_id: r.query_id, hits: findall(?h, hit(r.query_id, ?h))} in outer
      forget pending(r.query_id)
    }
  }
}
)vagent";

constexpr const char *transport122 = R"vagent(
event Payload { data: Str }
event SealedPayload { key_id: Str, data: Str }
event SecurityAlert { level: Int }
event OutgoingReport { recipient: Agent, content: Str }

capacity Transport {
  def send_payload(dest: Agent, data: Str)
}
skill Loopback implements Transport {
  def send_payload(dest: Agent, data: Str) { emit Payload{data: data} for dest }
}
skill Sealed(key_id: Str) implements Transport {
  def send_payload(dest: Agent, data: Str) {
    emit SealedPayload{key_id: key_id, data: data} for dest
  }
}

behavior Reporter {
  uses Transport
  on OutgoingReport as o { send_payload(o.recipient, o.content) }
}

agent Adaptive(alert_level: Int, key_id: Str) {
  install Loopback()
  behavior Reporter
  var sealed: Bool := false

  on SecurityAlert as a [a.level > alert_level & sealed == false] {
    install Sealed(key_id)
    sealed := true
  }
}
)vagent";

constexpr const char *hardware123 = R"vagent(
module mars:hardware

record Point { x: Float, y: Float }

event Position { at: Point }
event Battery { level: Float }

capacity Mobility {
  effect def follow(route: [Point]) within 10min   // fails if a leg loses traction
  effect preempts def halt() within 5s
  effect def dock() within 30min
}
capacity Science {
  effect def spectra(target_id: Str): Str within 1min
  effect def drill_core(target_id: Str) within 15min
}
capacity Routes {
  def direct(from_point: Point, to_point: Point): [Point]
  def ridge(from_point: Point, to_point: Point): [Point]
  def cost(route: [Point]): Float
}

// Its driver posts Position and Battery.
native skill Chassis implements Mobility
native skill Spectrometer implements Science
skill TerrainPlanner(leg_cost: Float) implements Routes {
  def direct(from_point: Point, to_point: Point): [Point] { [to_point] }
  def ridge(from_point: Point, to_point: Point): [Point] {
    [Point{x: from_point.x, y: to_point.y}, to_point]
  }
  def cost(route: [Point]): Float {
    let total := 0.0
    for _ in route do total := total + leg_cost
    total
  }
}
)vagent";

constexpr const char *exploration123 = R"vagent(
module mars:exploration
import std:agent
import std:goal (tab, match, upto)
import mars:hardware (Point, Position, Battery, Mobility, Science, Routes, Chassis, Spectrometer,
                      TerrainPlanner)

predicate position(at: Point)
predicate battery(level: Float)

event HazardDetected { kind: Str, severity: Int }
event MissionCommand { target_id: Str, location: Point }
event EmergencyHalted { kind: Str }
event ScienceLog { target_id: Str, mineral: Str }
event SampleSecured { target_id: Str, total: Int }
event SurveyFailed { target_id: Str, cause: Str }
event Heartbeat
event Alive { samples: Int }

goal Survey(target_id: Str, location: Point) perform
goal Reach(location: Point) achieve {
  succeed when believes position(location)
}
goal KeepCharged(floor: Float) maintain {
  holds believes battery(?level) from self & level > floor
}

agent MarsSurveyor(hazard_limit: Int, reserve_floor: Float, leg_cost: Float, settle: Duration,
                   heartbeat: Duration) {
  uses Mobility, Science, Routes
  install Chassis()
  install Spectrometer()
  install TerrainPlanner(leg_cost)
  var budget: Float := 0.0
  var samples: Int := 0
  var charging: Opt<Intention> := none
  var beat: Opt<Timer> := none

  on Initialize { beat := Schedules.periodic(heartbeat, Heartbeat{}) }
  on Heartbeat { emit Alive{samples: samples} }

  on Position as p { revise position(p.at) }
  on Battery as b {
    revise battery(b.level)
    if not charging then charging := adopt KeepCharged(reserve_floor)
  }

  on HazardDetected as h [h.severity > hazard_limit] {
    halt()
    for i in intentions() do
      if is_maintain(i) then { for r in restorers(i) do drop r } else drop i
    emit EmergencyHalted{kind: h.kind}
  }

  on MissionCommand as m { adopt Survey(m.target_id, m.location) }

  plan survey for Survey {
    achieve Reach(location) else {
      emit SurveyFailed{target_id: target_id, cause: cause}
      fail
    }
    let reading := spectra(target_id)
    let mineral := reading ? (tab(match("SPEC:")) & tab(upto(";")))
    emit ScienceLog{target_id: target_id, mineral: mineral}
    drill_core(target_id)
    samples := samples + 1
    emit SampleSecured{target_id: target_id, total: samples}
  }

  // Choose a route inside one statement: a failed arm restores budget.
  plan travel for Reach where believes position(?here) from self & believes battery(?level) from self {
    let route := (budget <- level - cost(direct(here, location))
                  & budget > reserve_floor & direct(here, location))
               | (budget <- level - cost(ridge(here, location))
                  & budget > reserve_floor & ridge(here, location))
    follow(route)
    await position(location) within settle
  }

  plan recharge for KeepCharged {
    dock()
    await battery(?level) [level > floor] within settle
  }
}
)vagent";

constexpr const char *depot124 = R"vagent(
module demo:depot
import std:agent

predicate delivered(order_id: Str)
question stock(item: Str): Int

event Order { order_id: Str, item: Str, count: Int }
event Shortage { item: Str, cause: Str }

goal Handle(order_id: Str, item: Str, count: Int) perform
goal Fetch(order_id: Str, item: Str, count: Int) perform

agent Warehouse(units: Int) {
  answer stock(item: Str): Int { units }
}

agent Courier {
  plan fetch for Fetch {
    // A real courier would act here; it tells its delegator what it carried.
    tell parent delivered(order_id)
  }
}

agent Dispatcher(units: Int, deadline: Duration) {
  var warehouse: Opt<Agent> := none
  var courier: Opt<Agent> := none

  on Initialize {
    warehouse := spawn Warehouse(units) in inner
    courier := spawn Courier() in inner
  }
  on Order as o { adopt Handle(o.order_id, o.item, o.count) }

  plan handle for Handle {
    let available := ask warehouse.stock(item) within deadline else 0
    if available < count then {
      emit Shortage{item: item, cause: "stock"}
      fail
    }
    achieve Fetch(order_id, item, count) by courier within deadline else {
      emit Shortage{item: item, cause: cause}
      fail
    }
    await delivered(order_id) within deadline
  }
}
)vagent";
// clang-format on

// -- helpers
// -------------------------------------------------------------------

std::string opName(EBinary::Op op) {
  switch (op) {
  case EBinary::Op::Mul:
    return "*";
  case EBinary::Op::Div:
    return "/";
  case EBinary::Op::Mod:
    return "%";
  case EBinary::Op::Add:
    return "+";
  case EBinary::Op::Sub:
    return "-";
  case EBinary::Op::Concat:
    return "++";
  case EBinary::Op::Lt:
    return "<";
  case EBinary::Op::Le:
    return "<=";
  case EBinary::Op::Gt:
    return ">";
  case EBinary::Op::Ge:
    return ">=";
  case EBinary::Op::Eq:
    return "==";
  case EBinary::Op::Ne:
    return "!=";
  case EBinary::Op::Alt:
    return "|";
  case EBinary::Op::Scan:
    return "?";
  case EBinary::Op::Assign:
    return ":=";
  case EBinary::Op::RevAssign:
    return "<-";
  case EBinary::Op::And:
    return "&";
  }
  return "??";
}

std::string show(const Expr &e);
std::string show(const ExprPtr &e) { return e ? show(*e) : "_"; }

/// An S-expression of the AST, covering the nodes the shape tests use.
std::string show(const Expr &e) {
  return std::visit(
      [](const auto &n) -> std::string {
        using T = std::decay_t<decltype(n)>;
        if constexpr (std::is_same_v<T, ELiteral>) {
          switch (n.kind) {
          case ELiteral::Kind::Int:
            return std::to_string(n.intValue);
          case ELiteral::Kind::Float:
            return "float";
          case ELiteral::Kind::Str:
            return "\"" + n.text + "\"";
          case ELiteral::Kind::Bool:
            return n.boolValue ? "true" : "false";
          case ELiteral::Kind::Duration:
            return std::to_string(n.durationNs) + "ns";
          case ELiteral::Kind::Unit:
            return "()";
          case ELiteral::Kind::None:
            return "none";
          }
          return "?";
        } else if constexpr (std::is_same_v<T, EName>) {
          return n.name;
        } else if constexpr (std::is_same_v<T, ESpecial>) {
          return "special";
        } else if constexpr (std::is_same_v<T, EUnary>) {
          const char *op = n.op == EUnary::Op::Not        ? "not"
                           : n.op == EUnary::Op::Neg      ? "neg"
                           : n.op == EUnary::Op::Generate ? "gen"
                                                          : "size";
          return std::string("(") + op + " " + show(n.operand) + ")";
        } else if constexpr (std::is_same_v<T, EBinary>) {
          return "(" + opName(n.op) + " " + show(n.left) + " " + show(n.right) +
                 ")";
        } else if constexpr (std::is_same_v<T, ERange>) {
          return "(to " + show(n.lo) + " " + show(n.hi) + " " + show(n.by) +
                 ")";
        } else if constexpr (std::is_same_v<T, ECall>) {
          std::string s = "(call " + show(n.callee);
          for (const auto &a : n.args) {
            s += " " + show(a);
          }
          return s + ")";
        } else if constexpr (std::is_same_v<T, EField>) {
          return "(. " + show(n.base) + " " + n.name + ")";
        } else if constexpr (std::is_same_v<T, ERecord>) {
          std::string s = n.type + "{";
          for (const auto &f : n.fields) {
            s += f.name + ":" + show(f.value) + ",";
          }
          return s + "}";
        } else if constexpr (std::is_same_v<T, EEmit>) {
          return "(emit " + show(n.event) + " in=" + show(n.space) +
                 " for=" + show(n.recipients) + ")";
        } else if constexpr (std::is_same_v<T, EFail>) {
          return "fail";
        } else if constexpr (std::is_same_v<T, EBlock>) {
          std::string s = "{";
          for (const auto &st : n.stmts) {
            s +=
                (st.isLet ? "let " + st.name + "=" : "") + show(st.value) + ";";
          }
          return s + "}";
        } else {
          return "?";
        }
      },
      e.node);
}

std::string shape(std::string_view source) {
  auto e = parseExpression(source);
  EXPECT_TRUE(e.has_value()) << (e ? "" : formatParseError(e.error()));
  return e ? show(**e) : "";
}

void expectParses(const std::string &source, const char *what) {
  auto u = parseUnit(source);
  EXPECT_TRUE(u.has_value())
      << what << ": " << (u ? "" : formatParseError(u.error()));
}

/// The statements of one handler body, so the newline rule can be seen in the
/// number of statements and their shapes.
std::vector<std::string> statements(const std::string &body) {
  const std::string source = "agent A {\n  on E {\n" + body + "\n  }\n}\n";
  auto u                   = parseUnit(source);
  EXPECT_TRUE(u.has_value()) << (u ? "" : formatParseError(u.error()));
  std::vector<std::string> out;
  if (!u) {
    return out;
  }
  const auto &agent   = std::get<AgentDecl>(u->decls.at(0));
  const auto &handler = std::get<HandlerDef>(agent.members.at(0));
  for (const auto &s : std::get<EBlock>(handler.body->node).stmts) {
    out.push_back(show(s.value));
  }
  return out;
}

ParseError refusal(const std::string &source) {
  auto u = parseUnit(source);
  EXPECT_FALSE(u.has_value()) << "expected a refusal for: " << source;
  return u ? ParseError{} : u.error();
}

// -- the design's own programs
// --------------------------------------------------

TEST(VagentParser, Fragment62Goals) { expectParses(goals62, "§6.2 goals"); }
TEST(VagentParser, Fragment62Plan) { expectParses(plan62, "§6.2 plan"); }

TEST(VagentParser, Fragment75Percept) {
  expectParses(std::string("agent Sensor {\n") + percept75 + "\n}\n",
               "§7.5 handler in an agent");
}

TEST(VagentParser, Fragment85Declarations) {
  expectParses(declarations85, "§8.5 declarations");
}

TEST(VagentParser, Example121ScatterGather) {
  expectParses(search121, "§12.1");
}
TEST(VagentParser, Example122SkillSwap) { expectParses(transport122, "§12.2"); }
TEST(VagentParser, Example123Hardware) {
  expectParses(hardware123, "§12.3 hardware");
}
TEST(VagentParser, Example123Exploration) {
  expectParses(exploration123, "§12.3 exploration");
}
TEST(VagentParser, Example124Depot) { expectParses(depot124, "§12.4"); }

TEST(VagentParser, Example121Structure) {
  auto u = parseUnit(search121);
  ASSERT_TRUE(u.has_value());
  ASSERT_TRUE(u->module.has_value());
  EXPECT_EQ(u->module->path, (std::vector<std::string>{"demo", "search"}));
  ASSERT_EQ(u->imports.size(), 1U);
  EXPECT_EQ(u->imports[0].path, (std::vector<std::string>{"std", "agent"}));
  // 3 predicates, 4 events, 2 agents.
  EXPECT_EQ(u->decls.size(), 9U);
}

TEST(VagentParser, Fragment85HandlerDetails) {
  auto u = parseUnit(declarations85);
  ASSERT_TRUE(u.has_value());
  const auto &behavior = std::get<BehaviorDecl>(u->decls.at(6));
  const auto &h        = std::get<HandlerDef>(behavior.members.at(1));
  const auto &trigger  = std::get<EventTrigger>(h.trigger);
  EXPECT_EQ(trigger.eventType, "CollisionWarning");
  EXPECT_EQ(trigger.binder, "w");
  EXPECT_EQ(h.priority, 1);
  EXPECT_TRUE(h.guard);

  const auto &agent  = std::get<AgentDecl>(u->decls.at(7));
  const auto &belief = std::get<HandlerDef>(agent.members.back());
  const auto &bt     = std::get<BeliefTrigger>(belief.trigger);
  EXPECT_TRUE(bt.added);
  EXPECT_EQ(bt.fact.name, "at");
  ASSERT_EQ(bt.fact.args.size(), 3U);
  EXPECT_EQ(bt.fact.args[0].logicVar, "who");
}

// -- precedence by AST shape (§8.4)
// ----------------------------------------------

TEST(VagentParser, ConjunctionIsLoosestAndAssignmentBindsTighter) {
  EXPECT_EQ(shape("budget <- b - cost & budget > floor & route"),
            "(& (& (<- budget (- b cost)) (> budget floor)) route)");
}

TEST(VagentParser, AlternationBindsLooserThanComparison) {
  EXPECT_EQ(shape("x == 1 | 2"), "(| (== x 1) 2)");
}

TEST(VagentParser, NotSitsBetweenComparisonAndAlternation) {
  EXPECT_EQ(shape("not a == b"), "(not (== a b))");
  EXPECT_EQ(shape("not a | b"), "(| (not a) b)");
}

TEST(VagentParser, ScanBindsTighterThanAssignment) {
  EXPECT_EQ(shape("x := s ? e"), "(:= x (? s e))");
}

TEST(VagentParser, AssignmentIsRightAssociative) {
  EXPECT_EQ(shape("a := b <- c"), "(:= a (<- b c))");
}

TEST(VagentParser, ActionOperandsArePostfixExpressions) {
  EXPECT_EQ(shape("emit E{a: 1} & next()"),
            "(& (emit E{a:1,} in=_ for=_) (call next))");
}

TEST(VagentParser, ArithmeticLevels) {
  EXPECT_EQ(shape("a + b * c ++ d"), "(++ (+ a (* b c)) d)");
  EXPECT_EQ(shape("-a * *xs"), "(* (neg a) (size xs))");
  EXPECT_EQ(shape("!xs"), "(gen xs)");
}

TEST(VagentParser, ToAndByBindLooserThanAlternation) {
  EXPECT_EQ(shape("1 to n - 1 by 2"), "(to 1 (- n 1) 2)");
}

TEST(VagentParser, PostfixBindsTightest) {
  EXPECT_EQ(shape("a.b(c).goal"), "(. (call (. a b) c) goal)");
}

TEST(VagentParser, ElseBindsToTheNearestUnmatchedForm) {
  auto e = parseExpression("if a then achieve G() else x");
  ASSERT_TRUE(e.has_value());
  const auto &i = std::get<EIf>((*e)->node);
  EXPECT_FALSE(i.elseBranch);
  EXPECT_TRUE(std::get<EAchieve>(i.thenBranch->node).orElse);

  auto braced = parseExpression("if a then { achieve G() } else x");
  ASSERT_TRUE(braced.has_value());
  EXPECT_TRUE(std::get<EIf>((*braced)->node).elseBranch);

  auto nested = parseExpression("if a then if b then x else y");
  ASSERT_TRUE(nested.has_value());
  const auto &outer = std::get<EIf>((*nested)->node);
  EXPECT_FALSE(outer.elseBranch);
  EXPECT_TRUE(std::get<EIf>(outer.thenBranch->node).elseBranch);
}

// -- record literals, terms and words
// ---------------------------------------------

TEST(VagentParser, PascalCaseBeforeBraceIsARecordLiteral) {
  EXPECT_EQ(shape("Point{x: 1, y: 2}"), "Point{x:1,y:2,}");
  EXPECT_EQ(shape("Tick{}"), "Tick{}");
}

TEST(VagentParser, RecordLiteralNeedsParenthesesInWhere) {
  auto bare = parseUnit("plan p for G where x == Foo { fail }");
  ASSERT_TRUE(bare.has_value()) << formatParseError(bare.error());
  const auto &plan = std::get<PlanDecl>(bare->decls.at(0));
  EXPECT_EQ(show(plan.where), "(== x Foo)");
  EXPECT_EQ(show(plan.body), "{fail;}");

  auto wrapped = parseUnit("plan p for G where x == (Foo{a: 1}) { fail }");
  ASSERT_TRUE(wrapped.has_value()) << formatParseError(wrapped.error());
  EXPECT_EQ(show(std::get<PlanDecl>(wrapped->decls.at(0)).where),
            "(== x Foo{a:1,})");
}

TEST(VagentParser, SnakeCaseBeforeBraceIsNeverARecord) {
  auto u = parseUnit("plan p for G where name { fail }");
  ASSERT_TRUE(u.has_value()) << formatParseError(u.error());
}

TEST(VagentParser, KeywordsAreWordsAfterDotAndInFieldsAndRecordKeys) {
  EXPECT_EQ(shape("f.goal"), "(. f goal)");
  EXPECT_EQ(shape("E{to: 1, goal: 2}"), "E{to:1,goal:2,}");
  expectParses("event Mail { to: Str, goal: Int }\n", "keyword field names");
  expectParses("import std:agent\nimport std:goal (tab, match)\n",
               "keywords in module paths");
}

TEST(VagentParser, LogicVariablesAndTerms) {
  auto u = parseUnit(R"(agent A {
  on E { believe at(self, 1, 2)
    for believes at(?w, ?x, ?y) do forget at(w, x, y) } })");
  ASSERT_TRUE(u.has_value()) << formatParseError(u.error());
}

TEST(VagentParser, DurationLiteralsCarryNanoseconds) {
  EXPECT_EQ(shape("250ms"), "250000000ns");
  EXPECT_EQ(shape("2s"), "2000000000ns");
  EXPECT_EQ(shape("5min"), "300000000000ns");
}

TEST(VagentParser, NumericAndStringLiterals) {
  EXPECT_EQ(shape("1_000 + 0x2A"), "(+ 1000 42)");
  EXPECT_EQ(shape(R"("a\tb\u{41}\\")"), "\"a\tbA\\\"");
  auto f = parseExpression("1e-3");
  ASSERT_TRUE(f.has_value());
  EXPECT_DOUBLE_EQ(std::get<ELiteral>((*f)->node).floatValue, 1e-3);
}

TEST(VagentParser, LeftArrowIsOneToken) {
  EXPECT_EQ(shape("a<-1"), "(<- a 1)");
  EXPECT_EQ(shape("a < -1"), "(< a (neg 1))");
}

TEST(VagentParser, CommentsAreWhitespace) {
  EXPECT_EQ(shape("a /* x\n y */ + // tail\n b"), "(+ a b)");
}

// -- the newline rule (§8.1)
// --------------------------------------------------------

TEST(VagentParser, LineBeginningWithBarContinues) {
  EXPECT_EQ(statements("x := a\n    | b"),
            (std::vector<std::string>{"(:= x (| a b))"}));
}

TEST(VagentParser, LineBeginningWithMinusStartsANewStatement) {
  EXPECT_EQ(statements("x := a\n    - b"),
            (std::vector<std::string>{"(:= x a)", "(neg b)"}));
}

TEST(VagentParser, LineBeginningWithParenStartsANewStatement) {
  EXPECT_EQ(statements("f\n    (g)"), (std::vector<std::string>{"f", "g"}));
}

TEST(VagentParser, OtherContinuationAndStartTokens) {
  EXPECT_EQ(statements("x := a\n + b"),
            (std::vector<std::string>{"(:= x (+ a b))"}));
  EXPECT_EQ(statements("x := a\n ++ b"),
            (std::vector<std::string>{"(:= x (++ a b))"}));
  EXPECT_EQ(statements("a\n & b"), (std::vector<std::string>{"(& a b)"}));
  EXPECT_EQ(statements("a\n < b"), (std::vector<std::string>{"(< a b)"}));
  EXPECT_EQ(statements("a\n .b"), (std::vector<std::string>{"(. a b)"}));
  EXPECT_EQ(statements("a\n * b"), (std::vector<std::string>{"a", "(size b)"}));
  EXPECT_EQ(statements("a\n ! b"), (std::vector<std::string>{"a", "(gen b)"}));
  EXPECT_EQ(statements("a\n [b]").size(), 2U);
}

TEST(VagentParser, TrailingOperatorContinuesTheLine) {
  EXPECT_EQ(statements("x := a &\n    b"),
            (std::vector<std::string>{"(& (:= x a) b)"}));
  EXPECT_EQ(statements("a &\n b"), (std::vector<std::string>{"(& a b)"}));
}

TEST(VagentParser, NewlinesInsideBracketsAreWhitespace) {
  EXPECT_EQ(statements("x := (a\n - b)"),
            (std::vector<std::string>{"(:= x (- a b))"}));
  EXPECT_EQ(statements("f(a,\n b)"),
            (std::vector<std::string>{"(call f a b)"}));
  EXPECT_EQ(statements("emit E{a: 1,\n b: 2}"),
            (std::vector<std::string>{"(emit E{a:1,b:2,} in=_ for=_)"}));
}

TEST(VagentParser, NewlinesAreSignificantAgainInsideANestedBlock) {
  EXPECT_EQ(statements("f({ a\n b })"),
            (std::vector<std::string>{"(call f {a;b;})"}));
}

TEST(VagentParser, SemicolonSeparatesStatementsOnOneLine) {
  EXPECT_EQ(statements("a; b ;c"), (std::vector<std::string>{"a", "b", "c"}));
}

TEST(VagentParser, TwoStatementsOnOneLineAreRefused) {
  const ParseError e = refusal("agent A { on E { a b } }");
  EXPECT_EQ(e.code, ParseErrorCode::MissingSeparator);
  EXPECT_EQ(e.loc.line, 1U);
  EXPECT_EQ(e.loc.column, 20U);
}

TEST(VagentParser, NewlinesOutsideBlocksAreWhitespace) {
  expectParses("agent\nA\n(x: Int,\n y: Int)\n{\n}\n"
               "plan p\nfor\nG\nwhere\nx > 1\n{ fail }\n",
               "declaration headers span lines");
}

TEST(VagentParser, LexerReportsNewlineBeforeEachToken) {
  auto t = Lexer("a\n b /* \n */ c d").tokenize();
  ASSERT_TRUE(t.has_value());
  ASSERT_EQ(t->size(), 5U);
  EXPECT_FALSE((*t)[0].newlineBefore);
  EXPECT_TRUE((*t)[1].newlineBefore);
  EXPECT_TRUE((*t)[2].newlineBefore);
  EXPECT_FALSE((*t)[3].newlineBefore);
}

TEST(VagentParser, NewlineSeparationRule) {
  EXPECT_TRUE(newlineSeparates(TokenKind::Identifier, TokenKind::Minus));
  EXPECT_TRUE(newlineSeparates(TokenKind::CloseParen, TokenKind::OpenParen));
  EXPECT_FALSE(newlineSeparates(TokenKind::Identifier, TokenKind::Bar));
  EXPECT_FALSE(newlineSeparates(TokenKind::Ampersand, TokenKind::Identifier));
  EXPECT_TRUE(newlineSeparates(TokenKind::Yield, TokenKind::Identifier));
}

// -- refusals with locations
// ----------------------------------------------------------

TEST(VagentParser, UnbalancedBraceIsLocatedAtTheOpener) {
  const ParseError e = refusal("agent A {\n  on E {\n    x := 1\n  }\n");
  EXPECT_EQ(e.code, ParseErrorCode::UnbalancedBrace);
  EXPECT_EQ(e.loc.line, 1U);
  EXPECT_EQ(e.loc.column, 9U);
}

TEST(VagentParser, StrayCloserIsRefused) {
  const ParseError e = refusal("agent A { }\n}\n");
  EXPECT_EQ(e.code, ParseErrorCode::UnbalancedBrace);
  EXPECT_EQ(e.loc.line, 2U);
}

TEST(VagentParser, MismatchedBracketIsRefused) {
  const ParseError e = refusal("agent A { on E { f(1] } }");
  EXPECT_EQ(e.code, ParseErrorCode::UnbalancedBracket);
  EXPECT_EQ(e.loc.line, 1U);
}

TEST(VagentParser, ComparisonDoesNotChain) {
  auto e = parseExpression("a < b < c");
  ASSERT_FALSE(e.has_value());
  EXPECT_EQ(e.error().code, ParseErrorCode::NonAssociativeComparison);
  EXPECT_EQ(e.error().loc.line, 1U);
  EXPECT_EQ(e.error().loc.column, 7U);
  EXPECT_NE(formatParseError(e.error()).find("1:7:"), std::string::npos);

  EXPECT_TRUE(parseExpression("a < b & b < c").has_value());
}

TEST(VagentParser, AskWithoutWithinIsRefused) {
  const ParseError e =
      refusal("agent A {\n  on E {\n    let x := ask b.q()\n  }\n}\n");
  EXPECT_EQ(e.code, ParseErrorCode::MissingWithin);
  EXPECT_EQ(e.loc.line, 4U);
}

TEST(VagentParser, AwaitAndAchieveByNeedWithin) {
  EXPECT_EQ(refusal("agent A { on E { await f(1) } }").code,
            ParseErrorCode::MissingWithin);
  EXPECT_EQ(refusal("agent A { on E { achieve G() by x } }").code,
            ParseErrorCode::MissingWithin);
}

TEST(VagentParser, LexicalErrorsAreLocated) {
  ParseError e = refusal("agent A { on E { x := \"abc } }");
  EXPECT_EQ(e.code, ParseErrorCode::LexicalError);
  EXPECT_EQ(e.loc.column, 23U);

  e = refusal("agent A { on E { x = 1 } }");
  EXPECT_EQ(e.code, ParseErrorCode::LexicalError);

  e = refusal("agent A { on E { x := 5parsecs } }");
  EXPECT_EQ(e.code, ParseErrorCode::LexicalError);

  e = refusal("/* never closed");
  EXPECT_EQ(e.code, ParseErrorCode::LexicalError);
}

TEST(VagentParser, MembersAreCheckedAgainstTheirContainer) {
  EXPECT_EQ(refusal("behavior B { install S() }").code,
            ParseErrorCode::NotAllowedHere);
  EXPECT_EQ(refusal("skill S implements C { on E { fail } }").code,
            ParseErrorCode::NotAllowedHere);
  expectParses("skill S implements C { on install { fail } }\n",
               "lifecycle in a skill");
}

TEST(VagentParser, ReservedWordCannotBeAParameter) {
  EXPECT_EQ(refusal("agent A(goal: Int) { }").code,
            ParseErrorCode::ExpectedName);
}

TEST(VagentParser, DeeplyNestedInputIsRefusedNotCrashed) {
  const std::string deep(5000, '(');
  auto e = parseExpression(deep + "1" + std::string(5000, ')'));
  ASSERT_FALSE(e.has_value());
  EXPECT_EQ(e.error().code, ParseErrorCode::NestingTooDeep);
}

} // namespace
