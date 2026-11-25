// M7a: the direct-host instance of the frontend conformance harness, plus the
// tests of the shared pieces themselves (managed submission cell, token
// routing, status gate, diagnostic registry) and the layering checks.

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

#include "io_frontend_conformance.hpp"

namespace {

using namespace xuzz_conformance;
using zigzag::DimVector;
using zigzag::vortex::OpcodeKind;
using zigzag::vortex::StepDisposition;

// ---- Direct Vortex: the shared-host baseline, no language at all ---------

/// Submit/match through the managed sum, then run a real Vortex program
/// (`Await request -> output; Halt`) on a VM cursor. Pending comes from the
/// VM's own Await opcode; the driver turns it into a Parked token and routes
/// the wake, exactly what a language runner does with its own activation.
class DirectVortexDriver final : public ConformanceDriver {
public:
  struct Activation final : FrontendActivation {
    CellRef cursor{};
    CellRef output{};
  };

  auto begin(World &w, const CapabilityCell capability,
             const CellRef specification) -> RunResult override {
    auto cell = w.bridge.submit(capability, specification);
    if (!cell) return RunResult::failed(cell.error());
    submission_        = *cell;
    const auto matched = w.bridge.match(*cell);
    if (!matched) return RunResult::failed(matched.error());
    if (const auto *refused = std::get_if<Refused>(&*matched)) {
      visible_.push_back(refused->outcome.cell);
      return RunResult::completed(refused->outcome);
    }
    const auto request = std::get<Admitted>(*matched).request;
    visible_.push_back(cell->cell);

    auto activation    = std::make_shared<Activation>();
    const auto await   = w.vm.mintOpcode(OpcodeKind::Await);
    const auto halt    = w.vm.mintOpcode(OpcodeKind::Halt);
    activation->output = w.arena.makeCell();
    w.core.bindInput(await, request.cell);
    w.core.bindOutput(await, activation->output);
    EXPECT_TRUE(w.arena.link(await, w.core.dims().spin, DimVector::POS, halt));
    activation->cursor = w.vm.spawnCursor(await);
    activation_        = activation;
    return drive(w, request);
  }

  auto resume(World &w, const FrontendToken token) -> RunResult override {
    return drive(w, token.request);
  }

  [[nodiscard]] auto visibleCells() const -> std::vector<CellRef> override {
    return visible_;
  }
  [[nodiscard]] auto submission() const
      -> std::optional<SubmissionCell> override {
    return submission_;
  }

private:
  auto drive(World &w, const RequestCell request) -> RunResult {
    const auto result = w.vm.run(activation_->cursor, 16);
    if (!result.success) {
      return RunResult::failed({"TEST_VM_FAILED", result.errorMessage, {}});
    }
    if (result.disposition == StepDisposition::Pending) {
      const auto &pending = *result.pending;
      FrontendToken token{.activation   = activation_,
                          .request      = request,
                          .waiter       = pending.waiter,
                          .continuation = pending.continuation};
      const auto routed = w.host.routeWake(token, *this);
      EXPECT_TRUE(routed.has_value() ||
                  routed.error() == IoHost::WakeRouteError::AlreadyRouted);
      return RunResult::parked(std::move(token));
    }
    const auto output = w.vm.cellReferenceOf(activation_->output);
    if (!output) return RunResult::failed({"TEST_NO_OUTPUT", "no output", {}});
    const auto incarnation = w.arena.incarnationOf(*output);
    const OutcomeCell outcome{.cell = *output, .incarnation = *incarnation};
    visible_.push_back(outcome.cell);
    return RunResult::completed(outcome);
  }

  std::shared_ptr<Activation> activation_;
  std::vector<CellRef> visible_;
  std::optional<SubmissionCell> submission_;
};

/// A language-free runner that awaits through FrontendBridge itself (what a
/// runner with no Await opcode, such as VLace's IO trampoline, would do).
class BridgeAwaitDriver final : public ConformanceDriver {
public:
  struct Activation final : FrontendActivation {
    CellRef cursor{};
  };

  auto begin(World &w, const CapabilityCell capability,
             const CellRef specification) -> RunResult override {
    auto cell = w.bridge.submit(capability, specification);
    if (!cell) return RunResult::failed(cell.error());
    submission_         = *cell;
    activation_         = std::make_shared<Activation>();
    activation_->cursor = w.vm.spawnCursor(w.vm.mintOpcode(OpcodeKind::Nop));
    visible_.push_back(cell->cell);
    // The language's own `match`: a refusal is bound and returned as a value
    // before any await is attempted.
    const auto matched = w.bridge.match(*cell);
    if (!matched) return RunResult::failed(matched.error());
    if (const auto *refused = std::get_if<Refused>(&*matched)) {
      visible_.push_back(refused->outcome.cell);
      return RunResult::completed(refused->outcome);
    }
    auto result = w.bridge.awaitSubmission(*cell, activation_->cursor, *this,
                                           activation_);
    note(result);
    return result;
  }
  auto resume(World &w, const FrontendToken token) -> RunResult override {
    auto result = w.bridge.resume(token, activation_->cursor, *this);
    note(result);
    return result;
  }
  [[nodiscard]] auto visibleCells() const -> std::vector<CellRef> override {
    return visible_;
  }
  [[nodiscard]] auto submission() const
      -> std::optional<SubmissionCell> override {
    return submission_;
  }

private:
  void note(const RunResult &result) {
    if (const auto outcome = detail::completed(result)) {
      visible_.push_back(outcome->cell);
    }
  }
  std::shared_ptr<Activation> activation_;
  std::vector<CellRef> visible_;
  std::optional<SubmissionCell> submission_;
};

const DriverFactory directHost = [](World &) {
  return std::make_unique<DirectVortexDriver>();
};
const DriverFactory bridgeAwait = [](World &) {
  return std::make_unique<BridgeAwaitDriver>();
};

XUZZ_FRONTEND_CONFORMANCE(IoFrontendConformanceDirectHost, directHost)
XUZZ_FRONTEND_CONFORMANCE(IoFrontendConformanceBridgeAwait, bridgeAwait)

// ---- The shared pieces ----------------------------------------------------

TEST(IoFrontendBridgeTest, SubmissionIsAManagedTaggedSumCell) {
  World w;
  const auto admitted = w.bridge.submit(w.capability, w.specification());
  ASSERT_TRUE(admitted);
  EXPECT_EQ(ioCellTag(w.arena, admitted->cell, admitted->incarnation),
            IoCellTag::Submission);
  EXPECT_EQ(submissionTag(w.arena, w.schema, *admitted),
            IoSubmissionTag::Admitted);
  const auto matched = w.bridge.match(*admitted);
  ASSERT_TRUE(matched);
  ASSERT_TRUE(std::holds_alternative<Admitted>(*matched));
  const auto request = std::get<Admitted>(*matched).request;
  // The accessors are exclusive: the wrong arm refuses.
  EXPECT_EQ(submissionOutcome(w.arena, w.schema, *admitted).error(),
            IoCellError::WrongTag);
  EXPECT_EQ(submissionRequest(w.arena, w.schema, *admitted), request);

  w.refuseAdmission  = true;
  const auto refused = w.bridge.submit(w.capability, w.specification());
  ASSERT_TRUE(refused);
  EXPECT_EQ(submissionTag(w.arena, w.schema, *refused),
            IoSubmissionTag::Refused);
  EXPECT_EQ(submissionRequest(w.arena, w.schema, *refused).error(),
            IoCellError::WrongTag);
  const auto outcome = submissionOutcome(w.arena, w.schema, *refused);
  ASSERT_TRUE(outcome);
  EXPECT_EQ(ioOutcomeTag(w.arena, *outcome), IoOutcomeTag::Rejected);
  EXPECT_EQ(w.submissions(), 1U) << "the refusal started no work";
}

TEST(IoFrontendBridgeTest, SubmissionCellsAreRuntimeOnlyAndReleasable) {
  World w;
  const auto cell = w.bridge.submit(w.capability, w.specification());
  ASSERT_TRUE(cell);
  const auto request = submissionRequest(w.arena, w.schema, *cell);
  ASSERT_TRUE(request);
  w.bridge.release(*cell);
  EXPECT_FALSE(ioCellTag(w.arena, cell->cell, cell->incarnation))
      << "a released submission is a stale handle";
  EXPECT_TRUE(ioCellTag(w.arena, request->cell, request->incarnation))
      << "the request it named is untouched";
  // With the sum's view gone the request is reapable again.
  w.complete(0, EndOfStream{{}});
  static_cast<void>(w.pump());
  ASSERT_TRUE(w.service.poll(*request).outcome);
  w.host.retireRequest(*request);
  EXPECT_FALSE(ioCellTag(w.arena, request->cell, request->incarnation));
}

TEST(IoFrontendBridgeTest, AwaitOnATerminalRequestIsCompletedWithoutParking) {
  World w;
  const auto cell = w.bridge.submit(w.capability, w.specification());
  ASSERT_TRUE(cell);
  w.complete(0, Unknown{});
  static_cast<void>(w.pump());
  struct Sink final : FrontendRunner {
    void wake(FrontendToken, FrontendWake) override { ++calls; }
    int calls{};
  } sink;
  const auto cursor  = w.vm.spawnCursor(w.vm.mintOpcode(OpcodeKind::Nop));
  const auto result  = w.bridge.awaitSubmission(*cell, cursor, sink, nullptr);
  const auto outcome = detail::completed(result);
  ASSERT_TRUE(outcome) << "Unknown is a terminal value, not a failure";
  EXPECT_EQ(ioOutcomeTag(w.arena, *outcome), IoOutcomeTag::Unknown);
  static_cast<void>(w.pump());
  EXPECT_EQ(sink.calls, 0);
}

TEST(IoFrontendBridgeTest, ReawaitWhilePendingKeepsOneContinuationAndRoute) {
  World w;
  const auto cell = w.bridge.submit(w.capability, w.specification());
  ASSERT_TRUE(cell);
  struct Sink final : FrontendRunner {
    void wake(FrontendToken token, FrontendWake) override {
      tokens.push_back(std::move(token));
    }
    std::vector<FrontendToken> tokens;
  } sink;
  const auto cursor = w.vm.spawnCursor(w.vm.mintOpcode(OpcodeKind::Nop));
  const auto first  = w.bridge.awaitSubmission(*cell, cursor, sink, nullptr, 3);
  const auto second = w.bridge.awaitSubmission(*cell, cursor, sink, nullptr, 3);
  const auto *a     = detail::parked(first);
  const auto *b     = detail::parked(second);
  ASSERT_TRUE(a && b);
  EXPECT_EQ(a->continuation, b->continuation);
  EXPECT_EQ(a->waiter, b->waiter);
  EXPECT_EQ(a->position, std::optional<std::size_t>{3});

  w.complete(0, EndOfStream{{}});
  static_cast<void>(w.pump());
  ASSERT_EQ(sink.tokens.size(), 1U) << "one wake for one parked await";
  EXPECT_EQ(sink.tokens[0].position, std::optional<std::size_t>{3});
}

TEST(IoFrontendBridgeTest, RouteWakeRejectsATokenTheHostNeverParked) {
  World w;
  const auto cell = w.bridge.submit(w.capability, w.specification());
  ASSERT_TRUE(cell);
  struct Sink final : FrontendRunner {
    void wake(FrontendToken, FrontendWake) override {}
  } sink;
  FrontendToken forged{.request = *submissionRequest(w.arena, w.schema, *cell),
                       .continuation = 42};
  const auto routed = w.host.routeWake(forged, sink);
  ASSERT_FALSE(routed);
  EXPECT_EQ(routed.error(), IoHost::WakeRouteError::NotParked);
}

TEST(IoFrontendBridgeTest, AWakeCanReawaitAndParkAgainFromInsideTheCallback) {
  World w;
  const auto first  = w.bridge.submit(w.capability, w.specification());
  const auto second = w.bridge.submit(w.capability, w.specification());
  ASSERT_TRUE(first && second);
  const auto cursor = w.vm.spawnCursor(w.vm.mintOpcode(OpcodeKind::Nop));
  struct Chain final : FrontendRunner {
    World *world{};
    SubmissionCell next;
    CellRef cursor{};
    std::optional<OutcomeCell> firstOutcome;
    std::optional<FrontendToken> reparked;
    void wake(FrontendToken token, FrontendWake) override {
      const auto done = world->bridge.resume(token, cursor, *this);
      firstOutcome    = detail::completed(done);
      const auto again =
          world->bridge.awaitSubmission(next, cursor, *this, nullptr);
      if (const auto *p = detail::parked(again)) reparked = *p;
    }
  } chain;
  chain.world  = &w;
  chain.next   = *second;
  chain.cursor = cursor;
  ASSERT_TRUE(detail::parked(
      w.bridge.awaitSubmission(*first, cursor, chain, nullptr, 0)));

  w.complete(0, EndOfStream{{}});
  static_cast<void>(w.pump());
  ASSERT_TRUE(chain.firstOutcome);
  ASSERT_TRUE(chain.reparked) << "the second request parked from the wake";
  EXPECT_EQ(chain.reparked->request,
            *submissionRequest(w.arena, w.schema, *second));
  w.complete(1, EndOfStream{{}});
  static_cast<void>(w.pump());
}

TEST(IoFrontendBridgeTest, StatusGateExposesPayloadsOnlyForCarryingTags) {
  for (const auto tag :
       {IoOutcomeTag::Complete, IoOutcomeTag::Partial, IoOutcomeTag::Failed,
        IoOutcomeTag::Cancelled, IoOutcomeTag::TimedOut}) {
    EXPECT_TRUE(tagMayCarryPayload(tag)) << toString(tag);
  }
  for (const auto tag :
       {IoOutcomeTag::Rejected, IoOutcomeTag::EndOfStream,
        IoOutcomeTag::Overflow, IoOutcomeTag::MessageTooLarge,
        IoOutcomeTag::Stale, IoOutcomeTag::Conflict, IoOutcomeTag::Unknown}) {
    EXPECT_FALSE(tagMayCarryPayload(tag)) << toString(tag);
  }
}

TEST(IoFrontendBridgeTest, GateRefusesAStaleOutcomeByDiagnostic) {
  World w;
  const auto outcome = w.service.refuse({IoErrorCode::Unsupported, 0});
  ASSERT_TRUE(w.bridge.gate(outcome));
  const auto stale = w.bridge.gate(OutcomeCell{outcome.cell, 0});
  ASSERT_FALSE(stale);
  EXPECT_EQ(stale.error().code, frontend_codes::tokenStale);
}

TEST(IoFrontendBridgeTest, AuditFlagsPlantedNativeIdentities) {
  World w;
  const auto root = w.arena.makeCell("request");
  const auto leak = w.arena.makeCell("continuation 7");
  ASSERT_TRUE(w.arena.link(root, w.schema.dim(IoDimension::State),
                           DimVector::POS, leak));
  const std::vector<CellRef> roots{root};
  const std::vector<std::uint64_t> forbidden{987654321};
  const auto findings = auditLanguageValue(w.arena, w.schema, roots, forbidden);
  ASSERT_EQ(findings.size(), 1U);
  EXPECT_EQ(findings[0].cell, leak);
  const auto raw = w.arena.makeCell("987654321");
  ASSERT_TRUE(w.arena.link(leak, w.schema.dim(IoDimension::State),
                           DimVector::POS, raw));
  EXPECT_EQ(auditLanguageValue(w.arena, w.schema, roots, forbidden).size(), 2U);
}

// ---- Diagnostic registry --------------------------------------------------

TEST(IoFrontendDiagnosticsTest, BuiltinsCoverTheSection10And14Refusals) {
  const auto registry = FrontendDiagnosticRegistry::withBuiltins();
  const std::vector<std::pair<std::string_view, FrontendId>> expected{
      {"IO_UNSUPPORTED_MODE", FrontendId::Shared},
      {"VQL_IO_QUERY_CONTEXT", FrontendId::Vql},
      {"VPL_IO_SHAPE_MISMATCH", FrontendId::Vpl},
      {"PROLOG_IO_EFFECT_CONTEXT", FrontendId::Vprolog},
      {"VLACE_ASYNC_UNAVAILABLE", FrontendId::Vlace},
      {"VAGENT_ACTOR_IO_SUSPEND_FORBIDDEN", FrontendId::Vagent},
  };
  for (const auto &[code, owner] : expected) {
    const auto entry = registry.find(code);
    ASSERT_TRUE(entry) << code;
    EXPECT_EQ(entry->owner, owner) << code;
    EXPECT_TRUE(registry.diagnose(code, "msg"));
  }
}

TEST(IoFrontendDiagnosticsTest, LanguagesRegisterTheirOwnAndNothingCollides) {
  auto registry = FrontendDiagnosticRegistry::withBuiltins();
  EXPECT_TRUE(registry.registerCode(FrontendId::Vicon, "VICON_IO_PURE_CONTEXT",
                                    "presentation cannot submit"));
  EXPECT_EQ(
      registry.registerCode(FrontendId::Vicon, "VICON_IO_PURE_CONTEXT", "again")
          .error(),
      DiagnosticRegistryError::Duplicate);
  EXPECT_EQ(
      registry.registerCode(FrontendId::Vicon, "VQL_IO_QUERY_CONTEXT", "x")
          .error(),
      DiagnosticRegistryError::WrongPrefix);
  EXPECT_EQ(
      registry.registerCode(FrontendId::Vicon, "VICON_lower", "x").error(),
      DiagnosticRegistryError::MalformedCode);
  EXPECT_EQ(registry.diagnose("VICON_TYPO", "x").error(),
            DiagnosticRegistryError::Unregistered);
  const auto diagnostic = registry.diagnose("VICON_IO_PURE_CONTEXT", "m", 4);
  ASSERT_TRUE(diagnostic);
  EXPECT_EQ(diagnostic->position, std::optional<std::size_t>{4});
}

// ---- Layering: no frontend branches in Core/VM ---------------------------

namespace fs = std::filesystem;

auto vortexSource(const std::string &name) -> std::string {
  const auto root =
      fs::path{__FILE__}.parent_path().parent_path().parent_path();
  const auto path = root / "apps/common/xanadu/vortex" / name;
  std::ifstream stream{path};
  EXPECT_TRUE(stream) << path;
  return {std::istreambuf_iterator<char>{stream}, {}};
}

auto lowered(std::string text) -> std::string {
  std::ranges::transform(text, text.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

TEST(IoFrontendLayeringTest, CoreAndVmNeverIncludeOrNameTheFrontendBridge) {
  const std::vector<std::string_view> forbidden{
      "frontend_bridge",   "frontend_token",     "frontend_diagnostics",
      "FrontendBridge",    "FrontendToken",      "FrontendRunner",
      "FrontendRunResult", "FrontendDiagnostic", "IoSubmission",
      "submissionTag",     "routeWake"};
  for (const auto *file : {"vortex_core.hpp", "vortex_core.cpp",
                           "vortex_vm.hpp", "vortex_vm.cpp"}) {
    const auto text = vortexSource(file);
    ASSERT_FALSE(text.empty()) << file;
    for (const auto name : forbidden) {
      EXPECT_EQ(text.find(name), std::string::npos) << file << ": " << name;
    }
  }
}

TEST(IoFrontendLayeringTest, TheAwaitPathHasNoLanguageBranches) {
  const auto text  = vortexSource("vortex_vm.cpp");
  const auto begin = text.find("VmExecutionResult VortexVM::stepAwait(");
  const auto end   = text.find("ExecutionResult VortexVM::stepImpl(");
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(end, std::string::npos);
  ASSERT_LT(begin, end);
  const auto awaitPath = lowered(text.substr(begin, end - begin));
  for (const auto name : {"vql", "vpl", "prolog", "vlace", "vicon", "vagent",
                          "frontend", "xuzz"}) {
    EXPECT_EQ(awaitPath.find(name), std::string::npos) << name;
  }
  const auto header = lowered(vortexSource("vortex_vm.hpp"));
  const auto at     = header.find("struct pendingawait");
  ASSERT_NE(at, std::string::npos);
  EXPECT_EQ(header.substr(at, 160).find("frontend"), std::string::npos);
}

TEST(IoFrontendLayeringTest, IoHostRoutesTokensWithoutNamingALanguage) {
  for (const auto *file : {"io_host.hpp", "io_host.cpp", "frontend_token.hpp",
                           "frontend_bridge.hpp", "frontend_bridge.cpp"}) {
    const auto text = lowered(vortexSource(file));
    ASSERT_FALSE(text.empty()) << file;
    for (const auto name : {"vqlquery", "vplexec", "prologquery", "vlaceio",
                            "viconrun", "vagentplan", "vql::", "vpl::"}) {
      EXPECT_EQ(text.find(name), std::string::npos) << file << ": " << name;
    }
  }
}

} // namespace
