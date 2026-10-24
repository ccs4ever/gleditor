/**
 * @file vagent_checker_test.cpp
 * @brief Tests for the vagent checker (design/vagent-agent-language.md §8).
 */
#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "common/xanadu/vagent/checker.hpp"
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

// -- helpers ----------------------------------------------------------------

std::vector<CheckError> errorsOf(const std::string &source) {
  auto unit = parseUnit(source);
  if (!unit) {
    ADD_FAILURE() << "parse error: " << formatParseError(unit.error()) << "\n"
                  << source;
    return {};
  }
  auto checked = check(*unit);
  return checked ? std::vector<CheckError>{} : checked.error();
}

std::string describe(const std::vector<CheckError> &errors) {
  std::string out;
  for (const auto &e : errors) {
    out += "  " + formatCheckError(e) + "\n";
  }
  return out.empty() ? "  (no errors)\n" : out;
}

void expectClean(const std::string &source, const char *what) {
  const auto errors = errorsOf(source);
  EXPECT_TRUE(errors.empty()) << what << ":\n" << describe(errors);
}

/// Exactly one error, with this code, on this line.
void expectOnly(const std::string &source, CheckErrorCode code,
                std::size_t line) {
  const auto errors = errorsOf(source);
  ASSERT_EQ(errors.size(), 1U) << describe(errors) << source;
  EXPECT_EQ(errors[0].code, code) << describe(errors);
  EXPECT_EQ(errors[0].loc.line, line) << describe(errors);
}

// Declarations the negative cases lean on; body line 1 is source line
// `preludeLines() + 1`.
constexpr const char *prelude = R"vagent(event Ping { n: Int, tag: Str }
event Pong extends Ping { extra: Float }
record Pt { x: Float, y: Float }
predicate seen(key id: Str, n: Int)
question depth(item: Str): Int
goal Work(job: Str, count: Int) perform
capacity Motor {
  effect def spin(rate: Float) within 5s
  def gear(n: Int): Int
}
capacity Plain { def noop() }
)vagent";

std::size_t preludeLines() {
  const std::string p = prelude;
  return static_cast<std::size_t>(std::count(p.begin(), p.end(), '\n'));
}

void neg(const std::string &body, CheckErrorCode code, std::size_t bodyLine) {
  expectOnly(std::string(prelude) + body, code, preludeLines() + bodyLine);
}

using C = CheckErrorCode;

// -- the design's own programs ----------------------------------------------

TEST(VagentChecker, Example121ScatterGather) {
  expectClean(search121, "§12.1");
}
TEST(VagentChecker, Example122SkillSwap) { expectClean(transport122, "§12.2"); }
TEST(VagentChecker, Example124Depot) { expectClean(depot124, "§12.4"); }
TEST(VagentChecker, Example123HardwareAlone) {
  expectClean(hardware123, "§12.3 hardware");
}

TEST(VagentChecker, Example123ExplorationWithImportsOpaque) {
  // Its declarations live in `mars:hardware`; imported names are not
  // type-checked here.
  expectClean(exploration123, "§12.3 exploration");
}

TEST(VagentChecker, Example123BothModulesInOneUnit) {
  // With the hardware declarations present the imported names resolve, so
  // effects, capacity methods and record fields are checked for real.
  const std::string hw = hardware123;
  const std::string ex = exploration123;
  const auto hwBody    = hw.substr(hw.find("record Point"));
  const auto firstDecl = ex.find("predicate position");
  const std::string all =
      ex.substr(0, firstDecl) + hwBody + ex.substr(firstDecl);
  auto unit = parseUnit(all);
  ASSERT_TRUE(unit.has_value()) << formatParseError(unit.error());
  auto checked = check(*unit);
  ASSERT_TRUE(checked.has_value()) << describe(checked.error());
  EXPECT_TRUE(checked->skills.at("Chassis").native);
  EXPECT_EQ(std::ranges::find(checked->opaqueImports, "Point"),
            checked->opaqueImports.end());
}

TEST(VagentChecker, Fragment85Declarations) {
  const std::string source = R"vagent(
event CollisionWarning { threat: Int, obstacle_id: Str }
event ObstacleEvaded { obstacle: Str }
event TargetLogged { target: Str }
event Relocated { x: Float, y: Float }
)vagent" + std::string(declarations85);
  expectClean(source, "§8.5");
}

TEST(VagentChecker, Fragment75Percept) {
  const std::string source = R"vagent(
predicate battery(level: Float)
event Battery { level: Float }
agent A {
)vagent" + std::string(percept75) +
                             "}\n";
  expectClean(source, "§7.5");
}

TEST(VagentChecker, Fragment62GoalsAndPlan) {
  const std::string source = R"vagent(
record Point { x: Float, y: Float }
predicate position(at: Point)
predicate battery(level: Float)
)vagent" + std::string(goals62) +
                             plan62;
  expectClean(source, "§6.2");
}

// -- symbol tables -----------------------------------------------------------

TEST(VagentChecker, SymbolTablesCarryFlattenedEventsAndSignatures) {
  const std::string source = std::string(prelude) + R"vagent(
event Deep extends Pong { more: Str }
agent A(limit: Int) {
  var total: Int := 0
  uses Motor
  def twice(x: Int): Int { x * 2 }
  answer depth(item: Str): Int { total }
}
)vagent";
  auto unit                = parseUnit(source);
  ASSERT_TRUE(unit.has_value());
  auto checked = check(*unit);
  ASSERT_TRUE(checked.has_value()) << describe(checked.error());
  EXPECT_EQ(checked->unit, &*unit);

  const auto &deep = checked->events.at("Deep");
  EXPECT_EQ(deep.supertypes, (std::vector<std::string>{"Pong", "Ping"}));
  ASSERT_EQ(deep.fields.size(), 4U);
  EXPECT_EQ(deep.fields[0].name, "n");
  EXPECT_EQ(deep.fields[3].name, "more");
  EXPECT_TRUE(checked->events.at("Initialize").builtin);

  const auto &a = checked->agents.at("A");
  ASSERT_EQ(a.params.size(), 1U);
  EXPECT_EQ(a.params[0].type.kind, Type::Kind::Int);
  ASSERT_EQ(a.methods.size(), 1U);
  EXPECT_TRUE(a.methods[0].hasResult);
  EXPECT_EQ(a.answers, std::vector<std::string>{"depth"});
  EXPECT_EQ(a.uses, std::vector<std::string>{"Motor"});

  const auto methods = allMethods(*checked, "Motor");
  ASSERT_EQ(methods.size(), 2U);
  EXPECT_TRUE(methods[0]->isEffect);
  EXPECT_EQ(methods[0]->withinMs, 5'000);
  EXPECT_EQ(checked->questions.at("depth").result.kind, Type::Kind::Int);
  EXPECT_TRUE(checked->predicates.at("seen").params[0].key);
  EXPECT_EQ(checked->goals.at("Work").params.size(), 2U);
}

TEST(VagentChecker, ImplicitEventFieldsAreReadable) {
  expectClean(std::string(prelude) + R"vagent(
agent A {
  on Ping as p {
    let who := p.source
    let t := p.time
    let o := p.ordinal + 1
    let s := p.space
    emit Ping{n: o, tag: "x"}
  }
}
)vagent",
              "implicit fields");
}

// -- accepted forms that look suspicious --------------------------------------

TEST(VagentChecker, ComparisonWithFalseIsAccepted) {
  expectClean(std::string(prelude) + R"vagent(
agent A {
  var sealed: Bool := false
  on Ping [sealed == false] { consume }
}
)vagent",
              "sealed == false");
}

TEST(VagentChecker, OptionalValuesStoreAndReadAsTheirElement) {
  expectClean(std::string(prelude) + R"vagent(
agent A {
  var who: Opt<Agent> := none
  var count: Opt<Int> := none
  on Ping as p {
    who := p.source
    count := 3
    if not who then count := none
    let n := count
  }
}
)vagent",
              "Opt");
}

TEST(VagentChecker, IfWithoutElseOfUnitBranchIsTotal) {
  expectClean(std::string(prelude) + R"vagent(
agent A(limit: Int) {
  var total: Int := 0
  on Ping {
    if limit > 0 then total := 1
    if limit > 1 then { total := 2 } else fail
  }
}
)vagent",
              "if");
}

TEST(VagentChecker, PureDefMayBeCalledFromACondition) {
  expectClean(std::string(prelude) + R"vagent(
agent A(limit: Int) {
  def big(x: Int): Int { x }
  on Ping as p [big(p.n) > limit] { consume }
}
)vagent",
              "pure def in a guard");
}

TEST(VagentChecker, PlanBodiesMaySuspendAtStatementsAndNestedBodies) {
  expectClean(std::string(prelude) + R"vagent(
agent A(limit: Int) {
  uses Motor
  plan p for Work {
    let d := ask self.depth(job) within 1s else 0
    if d > limit then {
      spin(1.0)
      await seen(job, ?k) within 2s
    } else yield
    achieve Work(job, count) else fail
  }
}
)vagent",
              "suspension positions");
}

TEST(VagentChecker, ElseOfAskSeesCause) {
  expectClean(std::string(prelude) + R"vagent(
agent A {
  plan p for Work {
    let d := ask self.depth(job) within 1s else {
      emit Ping{n: 1, tag: cause}
      fail
    }
  }
}
)vagent",
              "cause");
}

TEST(VagentChecker, AlternationOfLiteralsWarns) {
  const std::string source = std::string(prelude) + R"vagent(
agent A(limit: Int) {
  on Ping { limit == 1 | 2 }
}
)vagent";
  auto unit                = parseUnit(source);
  ASSERT_TRUE(unit.has_value());
  auto checked = check(*unit);
  ASSERT_TRUE(checked.has_value()) << describe(checked.error());
  ASSERT_EQ(checked->warnings.size(), 1U);
  EXPECT_EQ(checked->warnings[0].code, C::AlternationOfLiterals);
}

TEST(VagentChecker, ErrorsAreOrderedAndAllReported) {
  const auto errors = errorsOf(std::string(prelude) + R"vagent(
agent A {
  var x: Int := "a"
  on Ping { emit Ping{n: nope, tag: "t"} }
}
)vagent");
  ASSERT_EQ(errors.size(), 2U) << describe(errors);
  EXPECT_LT(errors[0].loc.line, errors[1].loc.line);
  EXPECT_EQ(errors[0].code, C::TypeMismatch);
  EXPECT_EQ(errors[1].code, C::UnknownName);
}

// -- names (§8.8) ------------------------------------------------------------

TEST(VagentCheckerNames, DuplicateField) {
  neg("agent A {\n  var x: Int := 1\n  var x: Int := 2\n}\n", C::DuplicateName,
      3);
}
TEST(VagentCheckerNames, DuplicateParameter) {
  neg("agent A(x: Int, x: Int) {\n}\n", C::DuplicateName, 1);
}
TEST(VagentCheckerNames, DuplicateTopLevelType) {
  neg("event Dup { }\nevent Dup { }\n", C::DuplicateName, 2);
}
TEST(VagentCheckerNames, DuplicateLocalInOneBlock) {
  neg("agent A {\n  on Ping {\n    let a := 1\n    let a := 2\n  }\n}\n",
      C::DuplicateName, 4);
}
TEST(VagentCheckerNames, UnknownName) {
  neg("agent A {\n  on Ping {\n    emit Ping{n: missing, tag: \"a\"}\n  }\n}\n",
      C::UnknownName, 3);
}
TEST(VagentCheckerNames, DollarNamesAreReserved) {
  neg("agent A {\n  var $x: Int := 1\n}\n", C::ReservedName, 2);
}
TEST(VagentCheckerNames, ValueNamesAreSnakeCase) {
  neg("agent A {\n  var Bad: Int := 1\n}\n", C::CaseConvention, 2);
}
TEST(VagentCheckerNames, TypeNamesArePascalCase) {
  neg("event lower { }\n", C::CaseConvention, 1);
}
TEST(VagentCheckerNames, UnknownType) {
  neg("agent A {\n  var p: Nope := 1\n}\n", C::UnknownType, 2);
}
TEST(VagentCheckerNames, UnknownEventInHandler) {
  neg("agent A {\n  on Nope { consume }\n}\n", C::UnknownType, 2);
}
TEST(VagentCheckerNames, UnknownField) {
  neg("agent A {\n  on Ping as p {\n    let q := p.zzz\n  }\n}\n",
      C::UnknownField, 3);
}
TEST(VagentCheckerNames, OccurrenceOutsideAHandler) {
  neg("agent A {\n  def f(): Int {\n    occurrence.n\n  }\n}\n",
      C::OccurrenceOutsideHandler, 3);
}
TEST(VagentCheckerNames, UnknownFunction) {
  neg("agent A {\n  on Ping {\n    nope(1)\n  }\n}\n", C::UnknownFunction, 3);
}
TEST(VagentCheckerNames, AmbiguousCapacityMethod) {
  neg("capacity Left { def go() }\ncapacity Right { def go() }\nagent A {\n"
      "  uses Left, Right\n  on Ping {\n    go()\n  }\n}\n",
      C::AmbiguousCall, 6);
}
TEST(VagentCheckerNames, UnknownCapacity) {
  neg("agent A {\n  uses Nope\n}\n", C::UnknownCapacity, 2);
}
TEST(VagentCheckerNames, UnknownSkill) {
  neg("agent A {\n  install Nope()\n}\n", C::UnknownSkill, 2);
}
TEST(VagentCheckerNames, UnknownBehavior) {
  neg("agent A {\n  behavior Nope\n}\n", C::UnknownBehavior, 2);
}
TEST(VagentCheckerNames, UnknownAgentInSpawn) {
  neg("agent A {\n  on Ping {\n    spawn Nope() in inner\n  }\n}\n",
      C::UnknownAgent, 3);
}
TEST(VagentCheckerNames, UnknownGoalInPlan) {
  neg("agent A {\n  plan p for Nope {\n    fail\n  }\n}\n", C::UnknownGoal, 2);
}
TEST(VagentCheckerNames, PlanParameterNotInGoal) {
  neg("agent A {\n  plan p for Work(nope) {\n    fail\n  }\n}\n",
      C::PlanParamNotInGoal, 2);
}
TEST(VagentCheckerNames, UnknownPredicate) {
  neg("agent A {\n  on Ping {\n    believe nope(1)\n  }\n}\n",
      C::UnknownPredicate, 3);
}
TEST(VagentCheckerNames, UnknownQuestion) {
  neg("agent A {\n  plan p for Work {\n    let d := ask self.nope(\"a\") "
      "within 1s\n  }\n}\n",
      C::UnknownQuestion, 3);
}
TEST(VagentCheckerNames, AnswerForAnUndeclaredQuestion) {
  neg("agent A {\n  answer zzz(): Int { 1 }\n}\n", C::UnknownQuestion, 2);
}
TEST(VagentCheckerNames, EventCycle) {
  neg("event A1 extends B1 { }\nevent B1 extends A1 { }\n", C::InheritanceCycle,
      1);
}
TEST(VagentCheckerNames, EventExtendingARecord) {
  neg("event E extends Pt { }\n", C::InvalidExtends, 1);
}

// -- types (§8.7)
// --------------------------------------------------------------

TEST(VagentCheckerTypes, FieldInitialiserMismatch) {
  neg("agent A {\n  var x: Int := \"s\"\n}\n", C::TypeMismatch, 2);
}
TEST(VagentCheckerTypes, NoneIsNotAnInt) {
  neg("agent A {\n  var x: Int := none\n}\n", C::TypeMismatch, 2);
}
TEST(VagentCheckerTypes, IntAndFloatDoNotMix) {
  neg("agent A {\n  on Ping {\n    let a := 1 + 2.0\n  }\n}\n", C::TypeMismatch,
      3);
}
TEST(VagentCheckerTypes, EqualityComparesOneType) {
  neg("agent A(limit: Int) {\n  on Ping [limit == 1.0] { consume }\n}\n",
      C::TypeMismatch, 2);
}
TEST(VagentCheckerTypes, CallArity) {
  neg("agent A {\n  uses Motor\n  on Ping {\n    let g := gear(1, 2)\n  }\n}\n",
      C::ArityMismatch, 4);
}
TEST(VagentCheckerTypes, SpawnArity) {
  neg("agent B(k: Int) { }\nagent A {\n  on Ping {\n    spawn B(1, 2) in "
      "inner\n  }\n}\n",
      C::ArityMismatch, 4);
}
TEST(VagentCheckerTypes, EmitMissingField) {
  neg("agent A {\n  on Ping {\n    emit Ping{n: 1}\n  }\n}\n", C::MissingField,
      3);
}
TEST(VagentCheckerTypes, EmitDuplicateField) {
  neg("agent A {\n  on Ping {\n    emit Ping{n: 1, n: 2, tag: \"a\"}\n  }\n}\n",
      C::DuplicateField, 3);
}
TEST(VagentCheckerTypes, EmitUnknownField) {
  neg("agent A {\n  on Ping {\n    emit Ping{n: 1, tag: \"a\", zzz: 3}\n  "
      "}\n}\n",
      C::UnknownField, 3);
}
TEST(VagentCheckerTypes, EmitFieldOfTheWrongType) {
  neg("agent A {\n  on Ping {\n    emit Ping{n: \"x\", tag: \"a\"}\n  }\n}\n",
      C::TypeMismatch, 3);
}
TEST(VagentCheckerTypes, EmitCannotSetAnImplicitField) {
  neg("agent A {\n  on Ping {\n    emit Ping{n: 1, tag: \"a\", source: self}\n"
      "  }\n}\n",
      C::ReservedField, 3);
}
TEST(VagentCheckerTypes, EventCannotDeclareAnImplicitField) {
  neg("event Bad { time: Int }\n", C::ReservedField, 1);
}
TEST(VagentCheckerTypes, ImplicitFieldIsNotAssignable) {
  neg("agent A {\n  on Ping as p {\n    let q := p\n    q.ordinal := 1\n  "
      "}\n}\n",
      C::ReservedField, 4);
}
TEST(VagentCheckerTypes, EmitOfARecord) {
  neg("agent A {\n  on Ping {\n    emit Pt{x: 1.0, y: 2.0}\n  }\n}\n",
      C::NotAnEvent, 3);
}
TEST(VagentCheckerTypes, EmitInNeedsASpace) {
  neg("agent A {\n  on Ping {\n    emit Ping{n: 1, tag: \"a\"} in 5\n  }\n}\n",
      C::TypeMismatch, 3);
}
TEST(VagentCheckerTypes, RecordLiteralFieldType) {
  neg("agent A {\n  on Ping {\n    let q := Pt{x: 1, y: 2.0}\n  }\n}\n",
      C::TypeMismatch, 3);
}
TEST(VagentCheckerTypes, PredicateArity) {
  neg("agent A {\n  on Ping {\n    believe seen(\"a\")\n  }\n}\n",
      C::ArityMismatch, 3);
}
TEST(VagentCheckerTypes, PredicateArgumentType) {
  neg("agent A {\n  on Ping {\n    believe seen(\"a\", \"b\")\n  }\n}\n",
      C::TypeMismatch, 3);
}
TEST(VagentCheckerTypes, AskElseHasTheAnswerType) {
  neg("agent A {\n  plan p for Work {\n    let d := ask self.depth(job) within "
      "1s else \"x\"\n  }\n}\n",
      C::TypeMismatch, 3);
}
TEST(VagentCheckerTypes, AnswerSignatureMustMatchTheQuestion) {
  neg("agent A {\n  answer depth(item: Int): Int { 1 }\n}\n",
      C::SignatureMismatch, 2);
}
TEST(VagentCheckerTypes, MethodBodyMatchesItsResult) {
  neg("agent A {\n  def f(): Int {\n    \"s\"\n  }\n}\n", C::TypeMismatch, 2);
}
TEST(VagentCheckerTypes, ListElementsAgree) {
  neg("agent A {\n  on Ping {\n    let l := [1, \"a\"]\n  }\n}\n",
      C::TypeMismatch, 3);
}
TEST(VagentCheckerTypes, ForIteratesListsAndRanges) {
  neg("agent A {\n  on Ping {\n    for x in 5.0 do consume\n  }\n}\n",
      C::TypeMismatch, 3);
}
TEST(VagentCheckerTypes, IfWithoutElseInAValuePosition) {
  neg("agent A(limit: Int) {\n  var total: Int := 0\n  on Ping {\n    let v := "
      "if limit > 0 then 5\n  }\n}\n",
      C::IfWithoutElse, 4);
}
TEST(VagentCheckerTypes, NotAPlace) {
  neg("agent A {\n  on Ping {\n    1 := 2\n  }\n}\n", C::NotAPlace, 3);
}
TEST(VagentCheckerTypes, AnEventBinderIsNotAPlace) {
  neg("agent A {\n  on Ping as p {\n    p := p\n  }\n}\n", C::NotAPlace, 3);
}
TEST(VagentCheckerTypes, NotCallable) {
  neg("agent A {\n  on Ping as p {\n    let v := p.n(1)\n  }\n}\n",
      C::NotCallable, 3);
}

// -- truth, failure and purity (§8.2)
// ------------------------------------------

TEST(VagentCheckerTruth, BoolConditionMustBeCompared) {
  neg("agent A {\n  var flag: Bool := false\n  on Ping {\n    if flag then "
      "fail\n  }\n}\n",
      C::BoolCondition, 4);
}
TEST(VagentCheckerTruth, BoolUnderNotMustBeCompared) {
  neg("agent A {\n  var flag: Bool := false\n  on Ping {\n    if not flag then "
      "fail\n  }\n}\n",
      C::BoolCondition, 4);
}
TEST(VagentCheckerTruth, BoolGuard) {
  neg("agent A {\n  var flag: Bool := false\n  on Ping [flag] { consume }\n}\n",
      C::BoolCondition, 3);
}
TEST(VagentCheckerTruth, GuardMayNotBelieve) {
  neg("agent A {\n  on Ping [believe seen(\"a\", 1)] {\n    consume\n  }\n}\n",
      C::ImpureCondition, 2);
}
TEST(VagentCheckerTruth, GuardMayNotCallAnEffect) {
  neg("agent A {\n  uses Motor\n  on Ping [spin(1.0)] { consume }\n}\n",
      C::ImpureCondition, 3);
}
TEST(VagentCheckerTruth, GuardMayNotCallAnImpureDef) {
  neg("agent A {\n  var total: Int := 0\n  def bump(): Int {\n    total := "
      "total + 1\n    total\n  }\n  on Ping [bump() == 1] {\n    consume\n  "
      "}\n}\n",
      C::ImpureCondition, 7);
}
TEST(VagentCheckerTruth, GoalClauseIsACondition) {
  neg("goal G2() achieve {\n  succeed when believe seen(\"a\", 1)\n}\n",
      C::ImpureCondition, 2);
}
TEST(VagentCheckerTruth, AnswerMayNotWriteAField) {
  neg("agent A {\n  var total: Int := 0\n  answer depth(item: Str): Int {\n    "
      "total := 1\n    1\n  }\n}\n",
      C::ImpureAnswer, 4);
}
TEST(VagentCheckerTruth, AnswerMayNotEmit) {
  neg("agent A {\n  answer depth(item: Str): Int {\n    emit Ping{n: 1, tag: "
      "\"a\"}\n    1\n  }\n}\n",
      C::ImpureAnswer, 3);
}
TEST(VagentCheckerTruth, AnswerIsReadOnlyThroughItsCallees) {
  neg("agent A {\n  var total: Int := 0\n  def bump(): Int {\n    total := "
      "total + 1\n    total\n  }\n  answer depth(item: Str): Int {\n    "
      "bump()\n  }\n}\n",
      C::ImpureAnswer, 8);
}

// -- positions (§8.3, §6.6)
// -----------------------------------------------------

TEST(VagentCheckerPositions, IrreversibleAssignmentNeedsABoundedPosition) {
  neg("agent A(limit: Int) {\n  var total: Int := 0\n  on Ping {\n    limit > "
      "0 "
      "& total := 5\n  }\n}\n",
      C::BoundedOnly, 4);
}
TEST(VagentCheckerPositions, ConsumeNeedsABoundedPosition) {
  neg("agent A(limit: Int) {\n  on Ping {\n    limit > 0 & consume\n  }\n}\n",
      C::BoundedOnly, 3);
}
TEST(VagentCheckerPositions, ReversibleAssignmentIsAllowedAnywhere) {
  expectClean(std::string(prelude) +
                  "agent A(limit: Int) {\n  var total: Int := 0\n  on Ping {\n"
                  "    limit > 0 & total <- 5\n  }\n}\n",
              "<- in an operand");
}
TEST(VagentCheckerPositions, AwaitInAHandler) {
  neg("agent A {\n  on Ping {\n    await seen(\"a\", 1) within 5s\n  }\n}\n",
      C::SuspensionOutsidePlan, 3);
}
TEST(VagentCheckerPositions, YieldInAHandler) {
  neg("agent A {\n  on Ping {\n    yield\n  }\n}\n", C::SuspensionOutsidePlan,
      3);
}
TEST(VagentCheckerPositions, SuspensionInsideAnExpression) {
  neg("agent A(limit: Int) {\n  plan p for Work {\n    achieve Work(\"a\", 1) "
      "& "
      "limit > 0\n  }\n}\n",
      C::SuspensionPosition, 3);
}
TEST(VagentCheckerPositions, SuspensionInAForBelievesLoop) {
  neg("agent A {\n  plan p for Work {\n    for believes seen(?i, ?k) do "
      "yield\n "
      " }\n}\n",
      C::SuspensionInBelievesLoop, 3);
}
TEST(VagentCheckerPositions, EffectInAMethod) {
  neg("agent A {\n  uses Motor\n  def run() {\n    spin(1.0)\n  }\n}\n",
      C::EffectInMethod, 4);
}
TEST(VagentCheckerPositions, EffectInAnExpressionOfAHandler) {
  neg("agent A(limit: Int) {\n  uses Motor\n  on Ping {\n    limit > 0 & "
      "spin(1.0)\n  }\n}\n",
      C::EffectPosition, 4);
}
TEST(VagentCheckerPositions, EffectInAnExpressionOfAPlan) {
  neg("agent A(limit: Int) {\n  uses Motor\n  plan p for Work {\n    limit > 0 "
      "& spin(1.0)\n  }\n}\n",
      C::EffectPosition, 4);
}
TEST(VagentCheckerPositions, EffectStatementInAHandlerIsStaged) {
  expectClean(std::string(prelude) +
                  "agent A {\n  uses Motor\n  on Ping { spin(1.0) }\n}\n",
              "effect statement");
}
TEST(VagentCheckerPositions, ReturnOutsideAMethod) {
  neg("agent A {\n  on Ping {\n    return\n  }\n}\n", C::ReturnOutsideMethod,
      3);
}
TEST(VagentCheckerPositions, ConsumeOutsideAHandler) {
  neg("agent A {\n  def f() {\n    consume\n  }\n}\n", C::ConsumeOutsideHandler,
      3);
}

// -- declarations (§8.9 notes)
// --------------------------------------------------

TEST(VagentCheckerDeclarations, EffectNeedsWithin) {
  neg("capacity Bad2 {\n  effect def go()\n}\n", C::EffectWithoutWithin, 2);
}
TEST(VagentCheckerDeclarations, EffectMethodWithABody) {
  neg("skill S implements Plain {\n  effect def noop() within 1s {\n  }\n}\n",
      C::EffectInSkill, 2);
}
TEST(VagentCheckerDeclarations, NonNativeSkillCannotImplementAnEffect) {
  neg("skill S implements Motor {\n  def gear(n: Int): Int { n }\n}\n",
      C::NonNativeEffect, 1);
}
TEST(VagentCheckerDeclarations, NativeSkillMayImplementAnEffect) {
  expectClean(std::string(prelude) + "native skill Spinner implements Motor\n",
              "native skill");
}
TEST(VagentCheckerDeclarations, SkillMustImplementEveryMethod) {
  neg("skill S implements Plain {\n}\n", C::MissingImplementation, 1);
}
TEST(VagentCheckerDeclarations, SkillSignatureMustMatch) {
  neg("skill S implements Plain {\n  def noop(x: Int) { }\n}\n",
      C::SignatureMismatch, 2);
}
TEST(VagentCheckerDeclarations, SkillImplementsUnknownCapacity) {
  neg("skill S implements Nope {\n}\n", C::UnknownCapacity, 1);
}

} // namespace
