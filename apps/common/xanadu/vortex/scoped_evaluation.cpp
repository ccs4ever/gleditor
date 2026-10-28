#include "common/xanadu/vortex/scoped_evaluation.hpp"
#include "common/xanadu/foreign_op_handle.hpp"
#include <cmath>
#include <limits>
namespace zigzag::vortex {
namespace {
[[noreturn]] void invalid(ScopedRefusal code, const char *message) {
  throw ScopeError(code, message);
}
void exact(const std::map<std::string, CellRef> &f,
           std::initializer_list<std::string_view> names) {
  if (f.size() != names.size())
    invalid(ScopedRefusal::InvalidSchema, "Unexpected scope record fields");
  for (auto name : names)
    if (!f.contains(std::string(name)))
      invalid(ScopedRefusal::InvalidSchema, "Missing scope record field");
}
ScopeType type(std::string_view name) {
  if (name == "Float") return ScopeType::Float;
  if (name == "Int") return ScopeType::Int;
  if (name == "Bool") return ScopeType::Bool;
  if (name == "Str") return ScopeType::Str;
  if (name == "Unit") return ScopeType::Unit;
  if (name == "Record") return ScopeType::Record;
  if (name == "List") return ScopeType::List;
  if (name == "PinnedRef") return ScopeType::PinnedRef;
  invalid(ScopedRefusal::InvalidSchema, "Unknown scope type");
}
} // namespace
std::string_view scopedRefusalName(ScopedRefusal code) {
  switch (code) {
  case ScopedRefusal::InvalidOperands:
    return "InvalidOperands";
  case ScopedRefusal::InvalidSchema:
    return "InvalidSchema";
  case ScopedRefusal::InvalidGraph:
    return "InvalidGraph";
  case ScopedRefusal::InvalidScopedCompletion:
    return "InvalidScopedCompletion";
  case ScopedRefusal::CutBelowFloor:
    return "CutBelowFloor";
  case ScopedRefusal::ResourceLimit:
    return "ResourceLimit";
  case ScopedRefusal::TypeMismatch:
    return "TypeMismatch";
  case ScopedRefusal::PublicationFailed:
    return "PublicationFailed";
  case ScopedRefusal::CallbackFault:
    return "CallbackFault";
  case ScopedRefusal::Cancelled:
    return "Cancelled";
  case ScopedRefusal::CursorOwnership:
    return "CursorOwnership";
  }
  return "InvalidScopedError";
}
void ScopeCodec::charge() {
  core_.arena().consumeWork();
  if (budget_->workRemaining == 0)
    invalid(ScopedRefusal::ResourceLimit, "Scope traversal limit");
  --budget_->workRemaining;
}
void ScopeCodec::debitBytes(std::size_t count) {
  if (count > budget_->bytesRemaining)
    invalid(ScopedRefusal::ResourceLimit, "Scope aggregate byte grant");
  budget_->bytesRemaining -= count;
}
std::string ScopeCodec::string(CellRef cell) {
  charge();
  auto &a = core_.arena();
  if (!a.contains(cell) || a.valueKindOf(cell) != xanadu::ValueKind::None)
    invalid(ScopedRefusal::TypeMismatch, "Scope string expected");
  seen_.insert(cell);
  if (core_.hasPipeline(cell))
    invalid(ScopedRefusal::InvalidGraph, "Scoped metadata has pipeline");
  // Bound before textOf allocates, including persistent spans.
  std::size_t size = 0;
  for (const auto &span : a.contentOf(cell)) {
    charge();
    if (span.length > limits_.bytes - size)
      invalid(ScopedRefusal::ResourceLimit, "Scope string limit");
    size += span.length;
  }
  debitBytes(size);
  return a.textOf(cell);
}
ScopeContent ScopeCodec::captureContent(CellRef cell) {
  charge();
  auto &a = core_.arena();
  seen_.insert(cell);
  ScopeContent result;
  bool stable  = false;
  bool scratch = false;
  if (a.contentOf(cell).size() > limits_.fields)
    invalid(ScopedRefusal::ResourceLimit, "Scope content segment grant");
  if (const auto source = a.contentSpaceOf(cell)) {
    charge();
    const auto space = a.spaceAt(*source);
    if (!space)
      invalid(ScopedRefusal::TypeMismatch,
              "Unavailable scope source namespace");
    result.source = ScopeSourcePin{*source, space->store, space->reader,
                                   space->manifold, space->ownedManifold};
  }
  for (const auto &span : a.contentOf(cell)) {
    charge();
    if (span.length > std::numeric_limits<std::size_t>::max())
      invalid(ScopedRefusal::ResourceLimit, "Scope content byte grant");
    debitBytes(static_cast<std::size_t>(span.length));
    if (span.scroll != xanadu::scratchScroll) {
      stable = true;
      result.parts.emplace_back(span);
      continue;
    }
    scratch         = true;
    const auto text = a.scratchTextOf(span);
    if (text.size() != span.length)
      invalid(ScopedRefusal::TypeMismatch, "Invalid scoped scratch span");
    if (!releaseScratchFloor_ ||
        (span.start <= *releaseScratchFloor_ &&
         span.length <= *releaseScratchFloor_ - span.start)) {
      result.parts.emplace_back(span);
    } else {
      const auto prefix =
          span.start < *releaseScratchFloor_
              ? *releaseScratchFloor_ - static_cast<std::size_t>(span.start)
              : 0;
      if (prefix)
        result.parts.emplace_back(xanadu::PrimediaSpan{
            span.scroll, span.start, static_cast<std::uint64_t>(prefix)});
      const auto suffix = text.substr(prefix);
      for (std::size_t n = 0; n < suffix.size(); ++n) charge();
      result.parts.emplace_back(std::string(suffix));
    }
  }
  if (stable && result.source && !result.source->store &&
      !result.source->reader)
    invalid(ScopedRefusal::TypeMismatch,
            "Scope source namespace has no reader");
  (void)scratch;
  return result;
}
void ScopeCodec::applyContent(CellRef cell, const ScopeContent &content) {
  auto &a = core_.arena();
  std::vector<xanadu::PrimediaSpan> spans;
  for (const auto &part : content.parts) {
    charge();
    if (const auto *span = std::get_if<xanadu::PrimediaSpan>(&part)) {
      spans.push_back(*span);
    } else {
      const auto &text = std::get<std::string>(part);
      for (std::size_t n = 0; n < text.size(); ++n) charge();
      debitBytes(text.size());
      spans.push_back(a.intern(text));
    }
  }
  if (!a.setContent(cell, spans))
    invalid(ScopedRefusal::PublicationFailed,
            "Cannot reconstruct scoped content");
}
CellRef ScopeCodec::restoreContent(const ScopeContent &content) {
  charge();
  auto &a = core_.arena();
  CellRef cell{};
  if (content.source) {
    const auto &pin  = *content.source;
    const auto space = a.spaceAt(pin.space);
    if (!space || space->store != pin.store || space->reader != pin.reader ||
        space->ownedManifold != pin.manifold || space->manifold != pin.view)
      invalid(ScopedRefusal::PublicationFailed, "Scope source pin changed");
    cell = a.makeQuote(pin.space, {});
  } else
    cell = a.makeCell();
  applyContent(cell, content);
  return cell;
}
CellRef ScopeCodec::reference(CellRef handle) {
  charge();
  auto &a = core_.arena();
  if (core_.hasPipeline(handle))
    invalid(ScopedRefusal::InvalidGraph,
            "Scope reference operand has pipeline");
  seen_.insert(handle);
  bool marked = false;
  for (const auto &edge : a.dimensionsOf(handle)) {
    charge();
    if (edge.pos != noCell &&
        string(edge.dim) == xanadu::cellRelocationDimension &&
        string(edge.pos) == xanadu::cellRelocationMarker)
      marked = true;
  }
  if (a.valueKindOf(handle) != xanadu::ValueKind::OpHandle || !marked)
    invalid(ScopedRefusal::InvalidOperands, "Marked OpHandle required");
  auto target = a.handleTarget(handle);
  if (!target || !a.contains(*target))
    invalid(ScopedRefusal::InvalidOperands, "Dead scope reference");
  return *target;
}
std::vector<CellRef> ScopeCodec::wing(CellRef opcode, DimVector side) {
  std::vector<CellRef> out;
  auto slots = core_.rawWingSlotsOf(opcode, side, limits_.fields);
  if (!slots) invalid(slots.error(), "Cyclic or oversized scope wing");
  for (auto slot : *slots) {
    charge();
    if (core_.hasPipeline(slot))
      invalid(ScopedRefusal::InvalidGraph, "Scoped operand has pipeline");
    out.push_back(reference(slot));
  }
  return out;
}
std::vector<CellRef> ScopeCodec::list(CellRef root) {
  if (string(root) != "[]")
    invalid(ScopedRefusal::InvalidSchema, "Scope list expected");
  std::vector<CellRef> out;
  std::unordered_set<CellRef> visited;
  auto node = core_.readLink(root, core_.dims().grab, DimVector::POS);
  while (node) {
    charge();
    seen_.insert(*node);
    if (!visited.insert(*node).second || out.size() >= limits_.fields)
      invalid(ScopedRefusal::InvalidSchema, "Cyclic or oversized scope list");
    out.push_back(*node);
    node = core_.readLink(*node, core_.dims().step, DimVector::POS);
  }
  return out;
}
std::map<std::string, CellRef> ScopeCodec::fields(CellRef root) {
  charge();
  seen_.insert(root);
  if (core_.hasPipeline(root))
    invalid(ScopedRefusal::InvalidGraph, "Scoped record has pipeline");
  std::map<std::string, CellRef> out;
  std::unordered_set<CellRef> visited;
  auto name = core_.readLink(root, core_.dims().vars, DimVector::POS);
  while (name) {
    charge();
    if (!visited.insert(*name).second || out.size() >= limits_.fields)
      invalid(ScopedRefusal::InvalidSchema, "Cyclic or oversized scope record");
    auto value = core_.readLink(*name, core_.dims().values, DimVector::POS);
    const auto label = string(*name);
    if (label.empty()) {
      if (value ||
          *name != core_.readLink(root, core_.dims().vars, DimVector::POS))
        invalid(ScopedRefusal::InvalidSchema, "Invalid record marker");
    } else {
      if (!value || !out.emplace(label, *value).second)
        invalid(ScopedRefusal::InvalidSchema,
                "Missing or duplicate scope field");
      seen_.insert(*value);
    }
    name = core_.readLink(*name, core_.dims().vars, DimVector::POS);
  }
  return out;
}
ScopeSchema ScopeCodec::schema(CellRef root, bool recordRoot) {
  struct Job {
    CellRef cell;
    bool record;
    std::size_t index;
    std::size_t depth;
  };
  ScopeSchema result;
  std::vector<Job> pending{{root, recordRoot, 0, 0}};
  while (!pending.empty()) {
    charge();
    const auto job = pending.back();
    pending.pop_back();
    if (job.depth > limits_.depth)
      invalid(ScopedRefusal::ResourceLimit, "Scope schema depth");
    auto f = fields(job.cell);
    ScopeSchemaNode node;
    const bool hasConstant = f.contains("constant");
    if (hasConstant) {
      const auto constant = core_.arena().asBool(f.at("constant"));
      if (!constant || core_.hasPipeline(f.at("constant")))
        invalid(ScopedRefusal::InvalidSchema, "Scope constant must be Bool");
      node.constant = *constant;
      f.erase("constant");
    }
    if (job.record) {
      if (string(job.cell) != "ScopeRecordSchema")
        invalid(ScopedRefusal::InvalidSchema, "Record schema expected");
      exact(f, {"fields"});
      node.type = ScopeType::Record;
    } else {
      if (string(job.cell) != "ScopeFieldSchema" || !f.contains("name") ||
          !f.contains("kind"))
        invalid(ScopedRefusal::InvalidSchema, "Field schema expected");
      node.name = string(f.at("name"));
      node.type = type(string(f.at("kind")));
    }
    const auto limit = [&](const char *name, std::size_t ceiling) {
      charge();
      const auto n = core_.arena().asInt64(f.at(name));
      if (!n || *n <= 0 || !std::in_range<std::size_t>(*n))
        invalid(ScopedRefusal::InvalidSchema, "Invalid schema bound");
      if (static_cast<std::uint64_t>(*n) > ceiling)
        invalid(ScopedRefusal::ResourceLimit, "Schema exceeds scope grant");
      if (core_.hasPipeline(f.at(name)))
        invalid(ScopedRefusal::InvalidGraph, "Scope schema bound has pipeline");
      return static_cast<std::size_t>(*n);
    };
    const auto child = [&](CellRef cell) {
      charge();
      if (job.depth >= limits_.depth || result.nodes.size() >= limits_.fields)
        invalid(ScopedRefusal::ResourceLimit, "Scope schema node grant");
      const auto index = result.nodes.size();
      result.nodes.emplace_back();
      pending.push_back({cell, false, index, job.depth + 1});
      return index;
    };
    switch (node.type) {
    case ScopeType::Record:
      if (!job.record) exact(f, {"name", "kind", "fields"});
      for (auto field : list(f.at("fields")))
        node.fields.push_back(child(field));
      break;
    case ScopeType::List:
      exact(f, {"name", "kind", "item", "max_items"});
      node.limit = limit("max_items", limits_.fields);
      node.item  = child(f.at("item"));
      break;
    case ScopeType::Str:
      exact(f, {"name", "kind", "max_bytes"});
      node.limit = limit("max_bytes", limits_.bytes);
      break;
    case ScopeType::PinnedRef:
      exact(f, {"name", "kind", "pinned_root"});
      node.pinnedRoot = string(f.at("pinned_root"));
      break;
    default:
      exact(f, {"name", "kind"});
      break;
    }
    if (hasConstant && node.type != ScopeType::Unit &&
        !(node.type == ScopeType::Record && node.fields.empty()))
      invalid(ScopedRefusal::InvalidSchema,
              "Only Unit or empty Record may declare a scope constant");
    result.nodes[job.index] = std::move(node);
  }
  for (const auto &node : result.nodes) {
    charge();
    std::unordered_set<std::string> names;
    for (auto index : node.fields) {
      charge();
      const auto &name = result.nodes[index].name;
      if (name.empty() || !names.insert(name).second)
        invalid(ScopedRefusal::InvalidSchema, "Duplicate schema field");
    }
  }
  return result;
}
ScopeDescriptor ScopeCodec::descriptor(CellRef root) {
  if (string(root) != "epistemic-scope-1")
    invalid(ScopedRefusal::InvalidSchema, "Unsupported scope schema version");
  auto f = fields(root);
  exact(f, {"args_schema", "result_schema", "effect_graph", "bindings"});
  ScopeDescriptor d;
  d.args   = schema(f.at("args_schema"), true);
  d.result = schema(f.at("result_schema"), true);
  std::unordered_set<CellRef> graph;
  for (auto handle : list(f.at("effect_graph"))) {
    const auto op = reference(handle);
    if (!graph.insert(op).second)
      invalid(ScopedRefusal::InvalidGraph, "Duplicate effect graph opcode");
    d.graph.push_back(op);
  }
  std::unordered_set<CellRef> regs;
  std::unordered_set<std::string> targets;
  for (auto binding : list(f.at("bindings"))) {
    if (string(binding) != "ScopeBinding")
      invalid(ScopedRefusal::InvalidSchema, "Scope binding expected");
    auto b = fields(binding);
    exact(b, {"register", "target", "kind"});
    ScopeBinding value{reference(b.at("register")), string(b.at("target")),
                       type(string(b.at("kind")))};
    if (!regs.insert(value.reg).second || !targets.insert(value.target).second)
      invalid(ScopedRefusal::InvalidSchema, "Duplicate scope binding");
    // The register target is checked separately; it is not descriptor data.
    d.bindings.push_back(std::move(value));
  }
  return d;
}
ScopeValue ScopeCodec::snapshot(CellRef value, const ScopeSchema &schema,
                                const std::map<std::string, CellRef> &pins,
                                std::vector<CellRef> *sourceCells) {
  struct Job {
    CellRef cell;
    std::size_t schema;
    std::size_t value;
    std::size_t depth;
  };
  ScopeValue result;
  result.nodes.emplace_back();
  if (sourceCells) sourceCells->clear();
  std::vector<Job> pending{{value, 0, 0, 0}};
  auto &a             = core_.arena();
  const auto mismatch = [] {
    invalid(ScopedRefusal::TypeMismatch, "Scope value does not match schema");
  };
  while (!pending.empty()) {
    charge();
    const auto job = pending.back();
    pending.pop_back();
    if (job.depth > limits_.depth)
      invalid(ScopedRefusal::ResourceLimit, "Scope value depth");
    if (core_.hasPipeline(job.cell))
      invalid(ScopedRefusal::InvalidGraph, "Scoped data leaf has pipeline");
    seen_.insert(job.cell);
    if (sourceCells) {
      if (sourceCells->size() < result.nodes.size())
        sourceCells->resize(result.nodes.size());
      (*sourceCells)[job.value] = job.cell;
    }
    const auto &s = schema.nodes.at(job.schema);
    ScopeValueNode node;
    node.schema      = job.schema;
    const auto child = [&](CellRef cell, std::size_t childSchema) {
      charge();
      if (job.depth >= limits_.depth || result.nodes.size() >= limits_.fields)
        invalid(ScopedRefusal::ResourceLimit, "Scope value node grant");
      const auto index = result.nodes.size();
      result.nodes.emplace_back();
      pending.push_back({cell, childSchema, index, job.depth + 1});
      return index;
    };
    switch (s.type) {
    case ScopeType::Float: {
      auto n = a.asDouble(job.cell);
      if (!n || !std::isfinite(*n)) mismatch();
      node.scalar = *n;
      break;
    }
    case ScopeType::Int: {
      auto n = a.asInt64(job.cell);
      if (!n) mismatch();
      node.scalar = *n;
      break;
    }
    case ScopeType::Bool: {
      auto n = a.asBool(job.cell);
      if (!n) mismatch();
      node.scalar = *n;
      break;
    }
    case ScopeType::Str: {
      if (a.valueKindOf(job.cell) != xanadu::ValueKind::None) mismatch();
      node.content       = captureContent(job.cell);
      std::size_t length = 0;
      for (const auto &part : node.content->parts) {
        charge();
        const auto n = std::holds_alternative<xanadu::PrimediaSpan>(part)
                           ? std::get<xanadu::PrimediaSpan>(part).length
                           : std::get<std::string>(part).size();
        if (n > s.limit - length) mismatch();
        length += static_cast<std::size_t>(n);
      }
      break;
    }
    case ScopeType::Unit:
      if (string(job.cell) != "()" || !fields(job.cell).empty()) mismatch();
      break;
    case ScopeType::PinnedRef: {
      const auto ref = reference(job.cell);
      auto pin       = pins.find(s.pinnedRoot);
      if (pin == pins.end() || pin->second != ref) mismatch();
      node.pinned = ref;
      break;
    }
    case ScopeType::Record: {
      if (a.valueKindOf(job.cell) != xanadu::ValueKind::None) mismatch();
      node.content = captureContent(job.cell);
      auto f       = fields(job.cell);
      if (f.size() != s.fields.size()) mismatch();
      for (auto index : s.fields) {
        charge();
        auto field = f.find(schema.nodes[index].name);
        if (field == f.end()) mismatch();
        node.children.push_back(child(field->second, index));
      }
      break;
    }
    case ScopeType::List: {
      node.content        = captureContent(job.cell);
      const auto elements = list(job.cell);
      if (elements.size() > s.limit || !s.item) mismatch();
      for (auto item : elements) node.children.push_back(child(item, *s.item));
      break;
    }
    }
    result.nodes[job.value] = std::move(node);
  }
  return result;
}
ScopeValue ScopeCodec::defaults(const ScopeSchema &schema,
                                const std::map<std::string, CellRef> &pins) {
  struct Job {
    std::size_t schema;
    std::size_t value;
  };
  ScopeValue result;
  result.nodes.emplace_back();
  std::vector<Job> pending{{0, 0}};
  while (!pending.empty()) {
    charge();
    const auto job = pending.back();
    pending.pop_back();
    const auto &s = schema.nodes.at(job.schema);
    ScopeValueNode node;
    node.schema = job.schema;
    switch (s.type) {
    case ScopeType::Float:
      node.scalar = 0.0;
      break;
    case ScopeType::Int:
      node.scalar = std::int64_t{0};
      break;
    case ScopeType::Bool:
      node.scalar = false;
      break;
    case ScopeType::Record:
      for (auto childSchema : s.fields) {
        charge();
        if (result.nodes.size() >= limits_.fields)
          invalid(ScopedRefusal::ResourceLimit, "Scope default node grant");
        const auto index = result.nodes.size();
        result.nodes.emplace_back();
        node.children.push_back(index);
        pending.push_back({childSchema, index});
      }
      break;
    case ScopeType::PinnedRef: {
      auto pin = pins.find(s.pinnedRoot);
      if (pin == pins.end())
        invalid(ScopedRefusal::InvalidSchema, "Unknown result argument pin");
      node.pinned = pin->second;
      break;
    }
    default:
      break;
    }
    result.nodes[job.value] = std::move(node);
  }
  return result;
}
void ScopeCodec::replaceRecord(
    CellRef root, const std::vector<std::pair<std::string, CellRef>> &values) {
  auto &a = core_.arena();
  if (!a.link(root, core_.dims().vars, DimVector::POS, std::nullopt))
    invalid(ScopedRefusal::PublicationFailed, "Cannot replace scope record");
  auto marker = a.makeCell("");
  if (!a.link(root, core_.dims().vars, DimVector::POS, marker))
    invalid(ScopedRefusal::PublicationFailed, "Cannot publish record marker");
  auto tail = marker;
  for (const auto &[name, value] : values) {
    charge();
    debitBytes(name.size());
    auto field = a.makeCell(name);
    if (!a.link(tail, core_.dims().vars, DimVector::POS, field) ||
        !a.link(field, core_.dims().values, DimVector::POS, value))
      invalid(ScopedRefusal::PublicationFailed, "Cannot publish scope field");
    tail = field;
  }
}
CellRef
ScopeCodec::record(std::string_view tag,
                   const std::vector<std::pair<std::string, CellRef>> &values) {
  charge();
  debitBytes(tag.size());
  auto root = core_.arena().makeCell(tag);
  replaceRecord(root, values);
  return root;
}
CellRef ScopeCodec::materialize(const ScopeValue &value,
                                const ScopeSchema &schema) {
  charge();
  if (value.nodes.empty() || value.nodes.size() > limits_.fields)
    invalid(ScopedRefusal::TypeMismatch, "Invalid scoped value tree");
  auto &a = core_.arena();
  std::vector<CellRef> cells(value.nodes.size());
  for (std::size_t i = value.nodes.size(); i > 0; --i) {
    charge();
    const auto &v = value.nodes[i - 1];
    const auto &s = schema.nodes.at(v.schema);
    CellRef root{};
    switch (s.type) {
    case ScopeType::Float:
      root = a.makeScalarCell(std::get<double>(v.scalar));
      break;
    case ScopeType::Int:
      root = a.makeScalarCell(std::get<std::int64_t>(v.scalar));
      break;
    case ScopeType::Bool:
      root = a.makeScalarCell(std::get<bool>(v.scalar));
      break;
    case ScopeType::Str:
      root = v.content ? restoreContent(*v.content)
                       : a.makeCell(std::get<std::string>(v.scalar));
      break;
    case ScopeType::Unit:
      root = a.makeCell("()");
      break;
    case ScopeType::PinnedRef: {
      root = a.makeCell();
      if (!v.pinned || !a.contains(*v.pinned) ||
          !a.setValueBits(root, xanadu::ValueKind::OpHandle, *v.pinned))
        invalid(ScopedRefusal::PublicationFailed, "Lost caller pin");
      auto marker = a.makeCell(xanadu::cellRelocationMarker);
      if (!a.link(root, a.ensureDimension(xanadu::cellRelocationDimension),
                  DimVector::POS, marker))
        invalid(ScopedRefusal::PublicationFailed, "Cannot mark reference");
      break;
    }
    case ScopeType::Record: {
      if (v.children.size() != s.fields.size())
        invalid(ScopedRefusal::TypeMismatch, "Invalid scoped record tree");
      std::vector<std::pair<std::string, CellRef>> fields;
      for (std::size_t n = 0; n < v.children.size(); ++n) {
        charge();
        if (v.children[n] < i || v.children[n] >= cells.size())
          invalid(ScopedRefusal::TypeMismatch,
                  "Scoped value edge is not forward");
        fields.emplace_back(schema.nodes.at(s.fields[n]).name,
                            cells[v.children[n]]);
      }
      root = v.content ? restoreContent(*v.content) : a.makeCell("ScopeRecord");
      replaceRecord(root, fields);
      break;
    }
    case ScopeType::List: {
      root       = v.content ? restoreContent(*v.content) : a.makeCell("[]");
      auto tail  = root;
      bool first = true;
      for (auto child : v.children) {
        charge();
        if (child < i || child >= cells.size())
          invalid(ScopedRefusal::TypeMismatch,
                  "Scoped value edge is not forward");
        if (!a.link(tail, first ? core_.dims().grab : core_.dims().step,
                    DimVector::POS, cells[child]))
          invalid(ScopedRefusal::PublicationFailed, "Cannot publish list");
        first = false;
        tail  = cells[child];
      }
      break;
    }
    }
    cells[i - 1] = root;
  }
  return cells[0];
}
} // namespace zigzag::vortex
