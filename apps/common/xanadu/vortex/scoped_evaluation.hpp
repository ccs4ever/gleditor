#ifndef COMMON_XANADU_VORTEX_SCOPED_EVALUATION_HPP
#define COMMON_XANADU_VORTEX_SCOPED_EVALUATION_HPP
#include "common/xanadu/vortex/vortex_core.hpp"
#include <map>
#include <memory>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
namespace zigzag::vortex {
[[nodiscard]] std::string_view scopedRefusalName(ScopedRefusal refusal);
// Supplied by the actor's validated host grant/settings. No implicit limit
// enables execution; the caller must deliberately admit a finite scope.
struct ScopedEvaluationLimits {
  std::size_t traversalWork{};
  std::size_t bytes{};
  std::size_t depth{};
  std::size_t fields{};
  std::size_t instructions{};
  std::size_t scopes{};
  [[nodiscard]] bool valid() const noexcept {
    return traversalWork && bytes && depth && fields && instructions && scopes;
  }
};
// Shared across nested scopes and every codec phase. Repeated metadata reads
// are conservatively charged again, bounding aggregate native carriers too.
struct ScopedEvaluationBudget {
  explicit ScopedEvaluationBudget(ScopedEvaluationLimits limits)
      : workRemaining(limits.traversalWork), bytesRemaining(limits.bytes) {}
  std::size_t workRemaining;
  std::size_t bytesRemaining;
};
enum class ScopedOutcome : std::uint8_t { Value, Empty, Refused };
class ScopeError : public std::runtime_error {
public:
  ScopeError(ScopedRefusal code, const char *message)
      : std::runtime_error(message), code(code) {}
  ScopedRefusal code;
};
enum class ScopeType : std::uint8_t {
  Float,
  Int,
  Bool,
  Str,
  Unit,
  Record,
  List,
  PinnedRef,
  // Binding-only: a read-only position in argument data or a caller pin.
  // Never a schema field, so it can be neither an argument nor a result.
  Cursor
};
struct ScopeSchemaNode {
  ScopeType type{ScopeType::Unit};
  std::string name;
  std::vector<std::size_t> fields;
  std::optional<std::size_t> item;
  std::size_t limit{};
  std::string pinnedRoot;
  bool constant{false};
};
// Flat edges bound construction and destruction independently of native stack.
struct ScopeSchema {
  std::vector<ScopeSchemaNode> nodes{ScopeSchemaNode{}};
  ScopeSchemaNode &root() { return nodes.at(0); }
  const ScopeSchemaNode &root() const { return nodes.at(0); }
};
struct ScopeSourcePin {
  std::uint32_t space{};
  const xanadu::Store *store{};
  const xanadu::SpanReader *reader{};
  const Manifold *view{};
  std::shared_ptr<const Manifold> manifold;
};
struct ScopeContent {
  // Stable source spans and pre-mark scratch keep their original identities.
  // Only scratch bytes invalidated by release are owned in native memory.
  std::vector<std::variant<xanadu::PrimediaSpan, std::string>> parts;
  std::optional<ScopeSourcePin> source;
};
// Owned values survive arena release; only validated caller pins retain refs.
struct ScopeValueNode {
  std::size_t schema{};
  CellValue scalar{std::string{}};
  std::optional<ScopeContent> content;
  std::vector<std::size_t> children;
  std::optional<CellRef> pinned;
};
struct ScopeValue {
  std::vector<ScopeValueNode> nodes;
};
struct ScopeBinding {
  CellRef reg{};
  std::string target;
  ScopeType type{};
};
struct ScopeDescriptor {
  ScopeSchema args;
  ScopeSchema result;
  std::vector<CellRef> graph;
  std::vector<ScopeBinding> bindings;
};
class ScopeCodec {
public:
  ScopeCodec(VortexCore &core, ScopedEvaluationLimits limits,
             std::optional<std::size_t> releaseScratchFloor = std::nullopt,
             std::shared_ptr<ScopedEvaluationBudget> budget = {})
      : core_(core), limits_(limits), releaseScratchFloor_(releaseScratchFloor),
        budget_(budget ? std::move(budget)
                       : std::make_shared<ScopedEvaluationBudget>(limits)) {
    if (!limits.valid())
      throw ScopeError(ScopedRefusal::ResourceLimit, "Invalid scope grant");
  }
  void charge();
  void debitBytes(std::size_t count);
  std::string boundedText(CellRef cell) { return string(cell); }
  CellRef reference(CellRef handle);
  std::vector<CellRef> wing(CellRef opcode, DimVector side);
  std::vector<CellRef> list(CellRef root);
  std::map<std::string, CellRef> fields(CellRef root);
  ScopeDescriptor descriptor(CellRef root);
  ScopeValue snapshot(CellRef value, const ScopeSchema &schema,
                      const std::map<std::string, CellRef> &pins = {},
                      std::vector<CellRef> *sourceCells          = nullptr);
  ScopeValue defaults(const ScopeSchema &schema,
                      const std::map<std::string, CellRef> &pins);
  CellRef materialize(const ScopeValue &value, const ScopeSchema &schema);
  CellRef record(std::string_view tag,
                 const std::vector<std::pair<std::string, CellRef>> &values);
  void
  replaceRecord(CellRef root,
                const std::vector<std::pair<std::string, CellRef>> &values);
  const std::unordered_set<CellRef> &protectedCells() const { return seen_; }
  // Caller cells a scope reached by walking from a named pin. A PinnedRef
  // value may name one of these under the same root as well as the pin.
  ScopeCodec *setPinnedReach(
      const std::unordered_multimap<CellRef, std::string> *reach) noexcept {
    reach_ = reach;
    return this;
  }

private:
  ScopeSchema schema(CellRef root, bool recordRoot);
  std::string string(CellRef cell);
  ScopeContent captureContent(CellRef cell);
  CellRef restoreContent(const ScopeContent &content);
  void applyContent(CellRef cell, const ScopeContent &content);
  VortexCore &core_;
  ScopedEvaluationLimits limits_;
  std::optional<std::size_t> releaseScratchFloor_;
  std::shared_ptr<ScopedEvaluationBudget> budget_;
  std::unordered_set<CellRef> seen_;
  const std::unordered_multimap<CellRef, std::string> *reach_{};
};
} // namespace zigzag::vortex
#endif
