#include "common/xanadu/vortex/mesh_evaluator.hpp"

#include "common/xanadu/bencode.hpp"
#include "common/xanadu/provenance.hpp"
#include "common/xanadu/vlace/lexer.hpp"
#include "common/xanadu/vpl/lexer.hpp"
#include "common/xanadu/vprolog/lexer.hpp"
#include "common/xanadu/vql/lexer.hpp"
#include <bit>
#include <charconv>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

#include "common/xanadu/vlace/compiler.hpp"
#include "common/xanadu/vlace/parser.hpp"
#include "common/xanadu/vlace/types.hpp"
#include "common/xanadu/vpl/vpl_engine.hpp"
#include "common/xanadu/vprolog/compiler.hpp"
#include "common/xanadu/vprolog/parser.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace zigzag::vortex::mesh {
namespace vpl     = xanadu::vpl;
namespace vql     = xanadu::vql;
namespace vprolog = xanadu::vprolog;
namespace vlace   = xanadu::vlace;
namespace {

template <class Lexer>
std::expected<void, std::string> checkTokens(std::string_view source,
                                             SyntaxLimits limits) {
  Lexer lexer(source);
  std::vector<char> brackets;
  for (std::size_t count = 0;; ++count) {
    auto token = lexer.nextToken();
    using Kind = decltype(token.kind);
    if (token.kind == Kind::EndOfFile) break;
    if (token.kind == Kind::Error)
      return std::unexpected("Invalid source token");
    if (count >= limits.tokens)
      return std::unexpected("Source token budget exhausted");
    std::optional<char> delimiter;
    if constexpr (requires { Kind::OpenParen; }) {
      if (token.kind == Kind::OpenParen) delimiter = '(';
      if (token.kind == Kind::CloseParen) delimiter = ')';
      if (token.kind == Kind::OpenBracket) delimiter = '[';
      if (token.kind == Kind::CloseBracket) delimiter = ']';
      if (token.kind == Kind::OpenBrace) delimiter = '{';
      if (token.kind == Kind::CloseBrace) delimiter = '}';
    } else {
      if (token.kind == Kind::LParen) delimiter = '(';
      if (token.kind == Kind::RParen) delimiter = ')';
      if (token.kind == Kind::LBracket) delimiter = '[';
      if (token.kind == Kind::RBracket) delimiter = ']';
    }
    if (!delimiter) continue;
    if (*delimiter == '(' || *delimiter == '[' || *delimiter == '{') {
      brackets.push_back(*delimiter);
      if (brackets.size() > limits.nesting)
        return std::unexpected("Source nesting budget exhausted");
    } else {
      const char opening = *delimiter == ')'   ? '('
                           : *delimiter == ']' ? '['
                                               : '{';
      if (brackets.empty() || brackets.back() != opening)
        return std::unexpected("Unbalanced source delimiters");
      brackets.pop_back();
    }
  }
  if (!brackets.empty()) return std::unexpected("Unbalanced source delimiters");
  return {};
}

std::expected<void, std::string>
checkSource(Frontend frontend, std::string_view source, SyntaxLimits limits) {
  if (source.size() > limits.bytes || !limits.tokens || !limits.nesting)
    return std::unexpected("Source parsing budget exhausted");
  // Use the language lexers so budget checks preserve normal quoting,
  // comments and operator spellings at the distributed boundary.
  switch (frontend) {
  case Frontend::Vpl:
    return checkTokens<vpl::Lexer>(source, limits);
  case Frontend::Vquery:
    return checkTokens<vql::Lexer>(source, limits);
  case Frontend::Vprolog:
    return checkTokens<vprolog::Lexer>(source, limits);
  case Frontend::Vlace:
    return checkTokens<vlace::Lexer>(source, limits);
  case Frontend::Vortex:
    return {};
  }
  return std::unexpected("Unknown frontend");
}

using Wire = xanadu::bencode::Value;

// The decoder is recursive; scan its complete token/depth cost without
// allocating a Value tree first. Byte strings are skipped as opaque payloads.
void artifactPreflight(std::string_view bytes, SyntaxLimits limits) {
  if (bytes.size() > limits.bytes || !limits.tokens || !limits.nesting)
    throw std::runtime_error("Vortex artifact decode budget exhausted");
  const auto tokenLimit =
      limits.tokens > std::numeric_limits<std::size_t>::max() / 32
          ? std::numeric_limits<std::size_t>::max()
          : limits.tokens * 32;
  std::size_t tokens = 0, depth = 0;
  for (std::size_t at = 0; at < bytes.size();) {
    if (++tokens > tokenLimit)
      throw std::runtime_error("Vortex artifact token budget exhausted");
    const char c = bytes[at++];
    if (c == 'l' || c == 'd') {
      if (++depth > std::min<std::size_t>(limits.nesting, 16))
        throw std::runtime_error("Vortex artifact nesting budget exhausted");
    } else if (c == 'e') {
      if (!depth--) throw std::runtime_error("Invalid Vortex artifact nesting");
    } else if (c == 'i') {
      const auto end = bytes.find('e', at);
      if (end == std::string_view::npos || end - at > 20)
        throw std::runtime_error("Invalid Vortex artifact integer");
      at = end + 1;
    } else if (c >= '0' && c <= '9') {
      const auto start = at - 1;
      const auto end   = bytes.find(':', start);
      std::size_t length{};
      if (end == std::string_view::npos)
        throw std::runtime_error("Invalid Vortex artifact string");
      const auto parsed =
          std::from_chars(bytes.data() + start, bytes.data() + end, length);
      if (parsed.ec != std::errc{} || parsed.ptr != bytes.data() + end ||
          length > bytes.size() - end - 1)
        throw std::runtime_error("Invalid Vortex artifact string length");
      at = end + 1 + length;
    } else
      throw std::runtime_error("Invalid Vortex artifact token");
  }
  if (depth) throw std::runtime_error("Incomplete Vortex artifact container");
}

const Wire &required(const Wire &value, std::string_view name) {
  const auto &fields = value.asDict();
  const auto found   = fields.find(std::string(name));
  if (found == fields.end())
    throw std::runtime_error("Missing Vortex artifact field");
  return found->second;
}

void dictionary(const Wire &value, std::size_t fields) {
  if (!value.isDict() || value.asDict().size() != fields)
    throw std::runtime_error("Invalid Vortex artifact dictionary");
}
std::size_t ordinal(const Wire &value) {
  const auto n = value.asInteger();
  if (n < 0 ||
      static_cast<std::uint64_t>(n) > std::numeric_limits<std::size_t>::max())
    throw std::runtime_error("Invalid Vortex artifact ordinal");
  return static_cast<std::size_t>(n);
}
Wire index(std::size_t value) {
  if (value >
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
    throw std::runtime_error("Vortex artifact ordinal overflow");
  return Wire::integer(static_cast<std::int64_t>(value));
}
Wire indices(const std::vector<std::size_t> &values) {
  xanadu::bencode::List result;
  for (auto value : values) result.push_back(index(value));
  return Wire::list(std::move(result));
}
std::vector<std::size_t> ordinals(const Wire &value) {
  std::vector<std::size_t> result;
  for (const auto &item : value.asList()) result.push_back(ordinal(item));
  return result;
}

// This explicit allowlist is the closed VM instruction set, whose effects are
// limited to its private arena. No stdlib/native callback or runIO descriptor
// can be installed through this artifact format.
bool pureOpcode(OpcodeKind opcode) {
  switch (opcode) {
  case OpcodeKind::Add:
  case OpcodeKind::Sub:
  case OpcodeKind::Mul:
  case OpcodeKind::Div:
  case OpcodeKind::DivMod:
  case OpcodeKind::Mod:
  case OpcodeKind::Neg:
  case OpcodeKind::Eq:
  case OpcodeKind::Neq:
  case OpcodeKind::Lt:
  case OpcodeKind::Lte:
  case OpcodeKind::Gt:
  case OpcodeKind::Gte:
  case OpcodeKind::And:
  case OpcodeKind::Or:
  case OpcodeKind::Not:
  case OpcodeKind::Link:
  case OpcodeKind::Break:
  case OpcodeKind::New:
  case OpcodeKind::Value:
  case OpcodeKind::Splice:
  case OpcodeKind::Clone:
  case OpcodeKind::Jump:
  case OpcodeKind::Branch:
  case OpcodeKind::Call:
  case OpcodeKind::Return:
  case OpcodeKind::Halt:
  case OpcodeKind::Nop:
  case OpcodeKind::Bind:
  case OpcodeKind::Resolve:
  case OpcodeKind::Assert:
  case OpcodeKind::Abs:
  case OpcodeKind::Min:
  case OpcodeKind::Max:
  case OpcodeKind::Clamp:
  case OpcodeKind::Trim:
  case OpcodeKind::ToLower:
  case OpcodeKind::ToUpper:
  case OpcodeKind::Unify:
  case OpcodeKind::IsVar:
  case OpcodeKind::MakeVar:
  case OpcodeKind::MakeTerm:
  case OpcodeKind::Deref:
  case OpcodeKind::Choice:
  case OpcodeKind::Fail:
  case OpcodeKind::Cut:
    return true;
  // Reads the host's choice stack, which a pure remote job does not have.
  case OpcodeKind::ChoiceDepth:
    return false;
  // Binds a handle to a cell of the local arena, which a pure remote job
  // has no business holding.
  case OpcodeKind::Point:
    return false;
  }
  return false;
}

void validateProgram(const VortexProgram &program, SyntaxLimits limits) {
  if (program.cells.empty() || program.cells.size() > limits.tokens ||
      program.instructions.empty() ||
      program.instructions.size() > limits.tokens ||
      program.outputs.size() > limits.tokens)
    throw std::runtime_error(
        "Vortex artifact cell/instruction budget exhausted");
  const auto cellIndex = [&](std::size_t cell) {
    if (cell >= program.cells.size())
      throw std::runtime_error("Vortex artifact reference is out of range");
  };
  const auto dimensionName = [](std::string_view name) {
    if (name.empty() || name.size() > 256 ||
        name.find('\0') != std::string_view::npos)
      throw std::runtime_error("Invalid Vortex artifact dimension name");
  };
  std::set<std::size_t> instructions;
  std::size_t edges = 0;
  const auto charge = [&] {
    if (++edges > limits.tokens)
      throw std::runtime_error("Vortex artifact edge budget exhausted");
  };
  for (const auto &instruction : program.instructions) {
    cellIndex(instruction.cell);
    if (std::holds_alternative<VortexReference>(
            program.cells[instruction.cell].value) ||
        std::holds_alternative<VortexDimension>(
            program.cells[instruction.cell].value))
      throw std::runtime_error(
          "Vortex instruction cell cannot alias a reference/dimension");
    if (!pureOpcode(instruction.opcode) ||
        !instructions.insert(instruction.cell).second)
      throw std::runtime_error(
          "Invalid, effectful or duplicate Vortex artifact instruction");
    for (auto input : instruction.inputs) {
      cellIndex(input);
      charge();
    }
    for (auto output : instruction.outputs) {
      cellIndex(output);
      charge();
    }
  }
  cellIndex(program.entry);
  if (!instructions.contains(program.entry))
    throw std::runtime_error("Vortex artifact entry is not an instruction");
  for (const auto &instruction : program.instructions) {
    if (instruction.next && !instructions.contains(*instruction.next))
      throw std::runtime_error(
          "Vortex artifact successor is not an instruction");
    std::optional<std::size_t> target;
    if (instruction.opcode == OpcodeKind::Jump ||
        instruction.opcode == OpcodeKind::Call ||
        instruction.opcode == OpcodeKind::Choice)
      target = 0;
    if (instruction.opcode == OpcodeKind::Branch) target = 1;
    if (target) {
      if (instruction.inputs.size() <= *target)
        throw std::runtime_error(
            "Vortex artifact control instruction has no target");
      const auto ref = std::get_if<VortexReference>(
          &program.cells[instruction.inputs[*target]].value);
      if (!ref || !instructions.contains(ref->cell))
        throw std::runtime_error("Vortex artifact control target is not a "
                                 "portable instruction reference");
    }
  }
  for (const auto &cell : program.cells) {
    if (auto ref = std::get_if<VortexReference>(&cell.value))
      cellIndex(ref->cell);
    if (auto dim = std::get_if<VortexDimension>(&cell.value))
      dimensionName(dim->name);
    if (auto literal = std::get_if<CellValue>(&cell.value))
      if (auto real = std::get_if<double>(literal);
          real && !std::isfinite(*real))
        throw std::runtime_error("Non-finite Vortex artifact literal");
    for (const auto &edge : cell.links) {
      charge();
      cellIndex(edge.target);
      dimensionName(edge.dimension);
      if (edge.direction != DimVector::POS && edge.direction != DimVector::NEG)
        throw std::runtime_error("Invalid Vortex artifact direction");
      if (edge.dimension == "d.spin" || edge.dimension == "d.grab" ||
          edge.dimension == "d.contract" || edge.dimension == "d.cache" ||
          edge.dimension == "d.stack")
        throw std::runtime_error(
            "Vortex artifact execution links require instruction descriptors");
    }
  }
  std::map<std::tuple<std::size_t, std::string, DimVector>, std::size_t> links;
  for (std::size_t i = 0; i < program.cells.size(); ++i)
    for (const auto &edge : program.cells[i].links)
      for (const auto &[key, target] :
           {std::pair{std::tuple{i, edge.dimension, edge.direction},
                      edge.target},
            std::pair{std::tuple{edge.target, edge.dimension, -edge.direction},
                      i}}) {
        const auto [found, inserted] = links.emplace(key, target);
        if (!inserted && found->second != target)
          throw std::runtime_error("Conflicting Vortex artifact graph edges");
      }
  for (const auto &output : program.outputs) cellIndex(output.cell);
}

Wire programWire(const VortexProgram &program) {
  xanadu::bencode::List cells;
  for (const auto &cell : program.cells) {
    Wire value;
    if (std::holds_alternative<std::monostate>(cell.value))
      value = Wire::list({Wire::string("empty")});
    else if (auto ref = std::get_if<VortexReference>(&cell.value))
      value = Wire::list({Wire::string("reference"), index(ref->cell)});
    else if (auto dim = std::get_if<VortexDimension>(&cell.value))
      value = Wire::list({Wire::string("dimension"), Wire::string(dim->name)});
    else
      value = std::visit(
          [](const auto &literal) -> Wire {
            using Type = std::remove_cvref_t<decltype(literal)>;
            if constexpr (std::same_as<Type, std::string>)
              return Wire::list({Wire::string("text"), Wire::string(literal)});
            else if constexpr (std::same_as<Type, double>)
              return Wire::list({Wire::string("double"),
                                 Wire::string(std::to_string(
                                     std::bit_cast<std::uint64_t>(literal)))});
            else if constexpr (std::same_as<Type, bool>)
              return Wire::list(
                  {Wire::string("bool"), Wire::integer(literal ? 1 : 0)});
            else
              return Wire::list({Wire::string("int"), Wire::integer(literal)});
          },
          std::get<CellValue>(cell.value));
    xanadu::bencode::List links;
    for (const auto &edge : cell.links)
      links.push_back(Wire::list(
          {Wire::string(edge.dimension), index(edge.target),
           Wire::integer(edge.direction == DimVector::POS ? 1 : -1)}));
    cells.push_back(Wire::dict({{"value", std::move(value)},
                                {"links", Wire::list(std::move(links))}}));
  }
  xanadu::bencode::List instructions;
  for (const auto &instruction : program.instructions) {
    auto next = instruction.next ? Wire::list({index(*instruction.next)})
                                 : Wire::list({});
    instructions.push_back(Wire::dict(
        {{"cell", index(instruction.cell)},
         {"opcode",
          Wire::integer(static_cast<std::int64_t>(instruction.opcode))},
         {"inputs", indices(instruction.inputs)},
         {"outputs", indices(instruction.outputs)},
         {"next", std::move(next)}}));
  }
  xanadu::bencode::List outputs;
  for (const auto &output : program.outputs)
    outputs.push_back(Wire::list(
        {index(output.cell), Wire::integer(output.dereference ? 1 : 0)}));
  return Wire::dict({{"cells", Wire::list(std::move(cells))},
                     {"instructions", Wire::list(std::move(instructions))},
                     {"entry", index(program.entry)},
                     {"outputs", Wire::list(std::move(outputs))}});
}

VortexProgram decodeProgram(std::string_view bytes, SyntaxLimits limits) {
  artifactPreflight(bytes, limits);
  const auto envelope = xanadu::bencode::decode(bytes);
  dictionary(envelope, 4);
  if (required(envelope, "format").asString() != "xanadu-vortex-instructions" ||
      required(envelope, "version").asInteger() != 1 ||
      envelope.encode() != bytes)
    throw std::runtime_error(
        "Invalid Vortex artifact format/version/canonical encoding");
  const auto &body = required(envelope, "body").asString();
  if (required(envelope, "sha256").asString() != xanadu::sha256Hex(body))
    throw std::runtime_error("Vortex artifact content hash mismatch");
  artifactPreflight(body, limits);
  const auto value = xanadu::bencode::decode(body);
  dictionary(value, 4);
  const auto &cells        = required(value, "cells").asList();
  const auto &instructions = required(value, "instructions").asList();
  if (cells.size() > limits.tokens || instructions.size() > limits.tokens)
    throw std::runtime_error(
        "Vortex artifact cell/instruction budget exhausted");
  VortexProgram program;
  program.entry = ordinal(required(value, "entry"));
  for (const auto &item : cells) {
    dictionary(item, 2);
    const auto &literal = required(item, "value").asList();
    if (literal.empty())
      throw std::runtime_error("Empty Vortex artifact value tag");
    const auto &tag = literal[0].asString();
    VortexProgramCell cell;
    if (tag == "empty" && literal.size() == 1)
      cell.value = std::monostate{};
    else if (literal.size() != 2)
      throw std::runtime_error("Invalid Vortex artifact value shape");
    else if (tag == "reference")
      cell.value = VortexReference{ordinal(literal[1])};
    else if (tag == "dimension")
      cell.value = VortexDimension{literal[1].asString()};
    else if (tag == "text")
      cell.value = CellValue{literal[1].asString()};
    else if (tag == "int")
      cell.value = CellValue{literal[1].asInteger()};
    else if (tag == "bool") {
      const auto boolean = literal[1].asInteger();
      if (boolean != 0 && boolean != 1)
        throw std::runtime_error("Invalid Vortex artifact boolean");
      cell.value = CellValue{boolean == 1};
    } else if (tag == "double") {
      const auto &bits = literal[1].asString();
      std::uint64_t number{};
      const auto parsed =
          std::from_chars(bits.data(), bits.data() + bits.size(), number);
      if (parsed.ec != std::errc{} || parsed.ptr != bits.data() + bits.size() ||
          std::to_string(number) != bits)
        throw std::runtime_error("Invalid Vortex artifact scalar bits");
      cell.value = CellValue{std::bit_cast<double>(number)};
    } else
      throw std::runtime_error("Unknown Vortex artifact value/native tag");
    for (const auto &edge : required(item, "links").asList()) {
      const auto &parts = edge.asList();
      if (parts.size() != 3)
        throw std::runtime_error("Invalid Vortex artifact link");
      auto direction = parts[2].asInteger();
      if (direction != 1 && direction != -1)
        throw std::runtime_error("Invalid Vortex artifact link direction");
      cell.links.push_back({parts[0].asString(), ordinal(parts[1]),
                            direction == 1 ? DimVector::POS : DimVector::NEG});
    }
    program.cells.push_back(std::move(cell));
  }
  for (const auto &item : instructions) {
    dictionary(item, 5);
    const auto opcode = required(item, "opcode").asInteger();
    if (opcode < 0 || opcode > static_cast<std::int64_t>(OpcodeKind::Cut))
      throw std::runtime_error("Unknown Vortex artifact opcode");
    const auto &next = required(item, "next").asList();
    if (next.size() > 1)
      throw std::runtime_error("Invalid Vortex artifact successor");
    program.instructions.push_back(
        {ordinal(required(item, "cell")), static_cast<OpcodeKind>(opcode),
         ordinals(required(item, "inputs")),
         ordinals(required(item, "outputs")),
         next.empty() ? std::nullopt
                      : std::optional<std::size_t>(ordinal(next[0]))});
  }
  for (const auto &item : required(value, "outputs").asList()) {
    const auto &parts = item.asList();
    if (parts.size() != 2 ||
        (parts[1].asInteger() != 0 && parts[1].asInteger() != 1))
      throw std::runtime_error("Invalid Vortex artifact output");
    program.outputs.push_back({ordinal(parts[0]), parts[1].asInteger() == 1});
  }
  validateProgram(program, limits);
  if (programWire(program).encode() != body)
    throw std::runtime_error("Noncanonical Vortex instruction graph");
  return program;
}

CellRef scalar(ArenaManifold &arena, const CellValue &value) {
  return std::visit(
      [&](const auto &v) {
        if constexpr (std::same_as<std::remove_cvref_t<decltype(v)>,
                                   std::string>)
          return arena.makeCell(v);
        else
          return arena.makeScalarCell(v);
      },
      value);
}

struct ResultBuilder {
  ArenaManifold &arena;
  DimRef items{arena.makeCell("d.mesh.items")};
  DimRef value{arena.makeCell("d.mesh.value")};
  DimRef arguments{arena.makeCell("d.mesh.arguments")};
  DimRef next{arena.makeCell("d.mesh.next")};

  void link(CellRef from, DimRef dim, CellRef to) {
    expectWritten(arena.link(from, dim, DimVector::POS, to));
  }
  void append(CellRef &tail, CellRef cell) {
    auto record = arena.makeCell();
    link(tail, items, record);
    link(record, value, cell);
    tail = record;
  }
};

// Refuse undefined integer arithmetic before entering the existing VM. The
// VM also accepts numeric text/bools, so match its conversions at this
// boundary.
void checkArithmetic(WorkerContext &context, CellRef instruction,
                     OpcodeKind opcode) {
  const auto inputs = context.core.inputsOf(instruction);
  std::vector<CellValue> values;
  for (auto input : inputs) values.push_back(context.core.render(input));
  const auto integer = [](const CellValue &value) -> std::int64_t {
    return std::visit(
        [](const auto &v) -> std::int64_t {
          using Type = std::remove_cvref_t<decltype(v)>;
          if constexpr (std::same_as<Type, double>) {
            if (!std::isfinite(v) || v < -9223372036854775808.0 ||
                v >= 9223372036854775808.0)
              throw std::runtime_error(
                  "Vortex artifact integer conversion overflow");
            return static_cast<std::int64_t>(v);
          } else if constexpr (std::same_as<Type, std::string>) {
            try {
              return std::stoll(v);
            } catch (...) {
              return 0;
            }
          } else
            return static_cast<std::int64_t>(v);
        },
        value);
  };
  for (const auto &value : values)
    if (auto real = std::get_if<double>(&value); real && !std::isfinite(*real))
      throw std::runtime_error("Vortex artifact non-finite operand");
  const bool realArithmetic =
      !values.empty() &&
      (std::holds_alternative<double>(values[0]) ||
       (values.size() > 1 && std::holds_alternative<double>(values[1])));
  const bool convertsInteger =
      opcode == OpcodeKind::Link || opcode == OpcodeKind::Break ||
      opcode == OpcodeKind::New || opcode == OpcodeKind::Value ||
      opcode == OpcodeKind::Splice || opcode == OpcodeKind::Jump ||
      opcode == OpcodeKind::Branch || opcode == OpcodeKind::Call ||
      opcode == OpcodeKind::Choice || opcode == OpcodeKind::Cut ||
      opcode == OpcodeKind::Bind;
  if (convertsInteger)
    for (const auto &value : values)
      if (std::holds_alternative<double>(value)) (void)integer(value);
  if (!values.empty() &&
      (opcode == OpcodeKind::Neg || opcode == OpcodeKind::Abs ||
       (opcode == OpcodeKind::Sub && values.size() == 1)) &&
      !std::holds_alternative<double>(values[0]) &&
      integer(values[0]) == std::numeric_limits<std::int64_t>::min())
    throw std::runtime_error("Vortex artifact integer arithmetic overflow");
  if (values.size() < 2) return;
  if (opcode == OpcodeKind::Add &&
      std::holds_alternative<std::string>(values[0]) &&
      std::holds_alternative<std::string>(values[1]))
    return;
  if (realArithmetic && opcode != OpcodeKind::DivMod &&
      opcode != OpcodeKind::Mod)
    return;
  const auto left = integer(values[0]), right = integer(values[1]);
  std::int64_t result{};
  bool overflow = false;
  switch (opcode) {
  case OpcodeKind::Add:
    overflow = __builtin_add_overflow(left, right, &result);
    break;
  case OpcodeKind::Sub:
    overflow = __builtin_sub_overflow(left, right, &result);
    break;
  case OpcodeKind::Mul:
    overflow = __builtin_mul_overflow(left, right, &result);
    break;
  case OpcodeKind::Div:
  case OpcodeKind::DivMod:
  case OpcodeKind::Mod:
    overflow = left == std::numeric_limits<std::int64_t>::min() && right == -1;
    break;
  default:
    break;
  }
  if (overflow)
    throw std::runtime_error("Vortex artifact integer arithmetic overflow");
}

void executeProgram(ResultBuilder &builder, WorkerContext &context,
                    const VortexProgram &program, CellRef &resultTail,
                    EvaluationBudget budget) {
  auto &arena = context.arena;
  std::map<std::string, DimRef> dimensions;
  const auto dimension = [&](const std::string &name) -> DimRef {
    if (auto existing = dimensions.find(name); existing != dimensions.end())
      return existing->second;
    auto existing     = arena.dimensionNamed(name);
    const auto result = existing != noCell ? existing : arena.makeCell(name);
    dimensions.emplace(name, result);
    return result;
  };
  std::vector<CellRef> cells;
  for (const auto &cell : program.cells) {
    if (auto literal = std::get_if<CellValue>(&cell.value))
      cells.push_back(scalar(arena, *literal));
    else if (auto dim = std::get_if<VortexDimension>(&cell.value))
      cells.push_back(dimension(dim->name));
    else
      cells.push_back(arena.makeCell());
  }
  std::vector<std::pair<CellRef, CellRef>> references;
  for (std::size_t i = 0; i < cells.size(); ++i) {
    if (auto ref = std::get_if<VortexReference>(&program.cells[i].value)) {
      expectWritten(arena.setValueBits(cells[i], xanadu::ValueKind::Int64,
                                       cells[ref->cell]));
      references.emplace_back(cells[i], cells[ref->cell]);
    }
  }
  using Edge = std::tuple<CellRef, DimRef, DimVector>;
  std::map<Edge, CellRef> edges;
  const auto edge = [&](CellRef from, DimRef dim, DimVector dir, CellRef to) {
    for (const auto &[key, target] : {std::pair{Edge{from, dim, dir}, to},
                                      std::pair{Edge{to, dim, -dir}, from}}) {
      auto [found, inserted] = edges.emplace(key, target);
      if (!inserted && found->second != target)
        throw std::runtime_error("Conflicting Vortex artifact graph edges");
    }
  };
  for (std::size_t i = 0; i < cells.size(); ++i)
    for (const auto &link : program.cells[i].links)
      edge(cells[i], dimension(link.dimension), link.direction,
           cells[link.target]);
  std::unordered_set<CellRef> instructions;
  for (const auto &instruction : program.instructions) {
    auto cell = cells[instruction.cell];
    if (!instructions.insert(cell).second)
      throw std::runtime_error("Aliased Vortex artifact instruction cells");
    context.vm.setOpcodeKind(cell, instruction.opcode);
    if (instruction.next)
      edge(cell, context.core.dims().spin, DimVector::POS,
           cells[*instruction.next]);
  }
  for (const auto &[key, target] : edges)
    expectWritten(arena.link(std::get<0>(key), std::get<1>(key),
                             std::get<2>(key), target));
  for (const auto &instruction : program.instructions) {
    const auto op = cells[instruction.cell];
    for (auto input : instruction.inputs) {
      const auto slot =
          ArenaManifold::refOf(static_cast<std::uint32_t>(arena.cellCount()));
      context.core.bindInput(op, cells[input]);
      references.emplace_back(slot, cells[input]);
    }
    for (auto output : instruction.outputs) {
      const auto slot =
          ArenaManifold::refOf(static_cast<std::uint32_t>(arena.cellCount()));
      context.core.bindOutput(op, cells[output]);
      references.emplace_back(slot, cells[output]);
    }
  }
  auto cursor = context.vm.spawnCursor(cells[program.entry], "mesh:artifact");
  std::size_t cycles = 0;
  for (;;) {
    auto pc = context.vm.getCursorOpcode(cursor);
    if (pc == noCell) break;
    if (!instructions.contains(pc))
      throw std::runtime_error("Vortex artifact attempted to execute outside "
                               "its sealed instructions");
    const auto opcode = context.vm.getOpcodeKind(pc);
    if (!opcode || !pureOpcode(*opcode))
      throw std::runtime_error(
          "Vortex artifact reached an invalid instruction");
    if (*opcode == OpcodeKind::Halt) break;
    if (cycles++ >= budget.maxCycles)
      throw std::runtime_error("Vortex artifact cycle budget exhausted");
    arena.consumeWork();
    checkArithmetic(context, pc, *opcode);
    auto executed = context.vm.step(cursor);
    if (!executed.success) throw std::runtime_error(executed.errorMessage);
  }
  context.vm.cut();
  for (const auto &output : program.outputs) {
    auto cell = cells[output.cell];
    if (output.dereference) {
      auto target = arena.asInt64(cell);
      if (!target || *target < 0 ||
          static_cast<std::uint64_t>(*target) >
              std::numeric_limits<CellRef>::max() ||
          !arena.contains(static_cast<CellRef>(*target)))
        throw std::runtime_error("Invalid Vortex artifact reference result");
      cell = static_cast<CellRef>(*target);
    }
    builder.append(resultTail, cell);
  }
  // Execution uses local numeric operands. Publication uses typed handles so
  // every reference is relocated again by the receiver rather than mistaken
  // for a portable integer literal.
  const auto relocation = arena.makeCell("d.mesh.relocation");
  for (const auto &[cell, originalTarget] : references) {
    (void)originalTarget;
    const auto target = arena.asInt64(cell);
    if (!target || *target < 0 ||
        static_cast<std::uint64_t>(*target) >
            std::numeric_limits<CellRef>::max() ||
        !arena.contains(static_cast<CellRef>(*target)))
      throw std::runtime_error(
          "Vortex artifact reference operand lost its target");
    expectWritten(arena.setValueBits(cell, xanadu::ValueKind::OpHandle,
                                     static_cast<CellRef>(*target)));
    builder.link(cell, relocation, arena.makeCell("cell"));
  }
}

std::optional<CellRef> vplCell(ResultBuilder &builder, const vpl::VplView &view,
                               CellRef metadata, std::size_t depth = 0) {
  auto &arena = builder.arena;
  arena.consumeWork();
  if (depth >= 64)
    throw std::runtime_error("VPL enclosure nesting budget exhausted");
  if (view.isEnclosed()) {
    if (!view.enclosedView())
      throw std::runtime_error("VPL enclosure has no value");
    auto box      = arena.makeCell("vpl:enclosed");
    auto enclosed = arena.makeCell("d.mesh.enclosed");
    auto nested   = arena.makeCell("vpl:view");
    builder.link(box, enclosed, nested);
    auto value = vplCell(builder, *view.enclosedView(), nested, depth + 1);
    if (value) builder.link(nested, builder.value, *value);
    return box;
  }
  if (!view.axes().empty()) {
    auto axes        = arena.makeCell("d.mesh.axes");
    auto dimension   = arena.makeCell("d.mesh.axis");
    auto extent      = arena.makeCell("d.mesh.extent");
    auto direction   = arena.makeCell("d.mesh.direction");
    CellRef axisTail = metadata;
    for (std::size_t i = 0; i < view.axes().size(); ++i) {
      auto axis = arena.makeCell();
      builder.link(axisTail, axes, axis);
      builder.link(axis, dimension, view.axes()[i].dim);
      builder.link(axis, direction,
                   arena.makeScalarCell(view.axes()[i].dir == DimVector::POS));
      if (i < view.extents().size())
        builder.link(
            axis, extent,
            arena.makeScalarCell(static_cast<std::int64_t>(view.extents()[i])));
      axisTail = axis;
    }
  }
  if (view.isScalar()) {
    if (view.isString()) return arena.makeCell(view.scalarString());
    if (view.isFloat()) return arena.makeScalarCell(view.scalarFloat());
    return arena.makeScalarCell(view.scalarInt());
  }
  return view.origin() == noCell ? std::nullopt
                                 : std::optional<CellRef>(view.origin());
}

// Resolution rolls the arena back after each solution, so retaining a CellRef
// from the callback would retain a dangling name. Capture values before undo.
struct LogicValue {
  CellValue value;
  std::vector<LogicValue> arguments;
};
LogicValue captureLogic(vprolog::Compiler &compiler, CellRef ref,
                        std::unordered_set<CellRef> &active, std::size_t &cells,
                        std::size_t limit) {
  auto &arena = compiler.core().arena();
  arena.consumeWork();
  if (++cells > limit || active.size() >= 512)
    throw std::runtime_error("Prolog result cell budget exhausted");
  ref = compiler.stdlib().deref(ref);
  if (!active.insert(ref).second)
    throw std::runtime_error("Cyclic Prolog result is not portable");
  LogicValue result{.value = compiler.core().render(ref), .arguments = {}};
  const auto args = compiler.stdlib().argumentsOf(ref);
  if (!args.empty()) {
    result.value = compiler.stdlib().functorOf(ref);
    for (auto arg : args)
      result.arguments.push_back(
          captureLogic(compiler, arg, active, cells, limit));
  }
  active.erase(ref);
  return result;
}
CellRef materializeLogic(ResultBuilder &builder, const LogicValue &v) {
  auto root    = scalar(builder.arena, v.value);
  CellRef tail = root;
  for (const auto &arg : v.arguments) {
    auto record = builder.arena.makeCell();
    builder.link(tail, builder.arguments, record);
    builder.link(record, builder.value, materializeLogic(builder, arg));
    tail = record;
  }
  return root;
}

CellRef materializeLace(ResultBuilder &builder, vlace::Compiler &compiler,
                        vlace::ValuePtr raw,
                        std::unordered_map<const vlace::Value *, CellRef> &seen,
                        std::size_t depth = 0) {
  builder.arena.consumeWork();
  if (depth >= 512)
    throw std::runtime_error("Vlace result nesting budget exhausted");
  auto forced = compiler.forceValue(std::move(raw));
  if (!forced) throw std::runtime_error(forced.error().detail);
  const auto &v = *forced;
  if (!v) throw std::runtime_error("Empty Vlace result");
  if (auto found = seen.find(v.get()); found != seen.end())
    return found->second;
  using Kind = vlace::Value::Kind;
  switch (v->kind) {
  case Kind::Int:
    return builder.arena.makeScalarCell(v->i);
  case Kind::Float:
    return builder.arena.makeScalarCell(v->f);
  case Kind::Str:
    return builder.arena.makeCell(v->s);
  case Kind::Bool:
    return builder.arena.makeScalarCell(v->b);
  case Kind::Ctor: {
    if (v->args.size() != v->ctorArity || v->tag.starts_with("IO."))
      throw std::runtime_error(
          "Effect or partial constructor is not a portable result");
    auto root = builder.arena.makeCell(v->tag);
    seen.emplace(v.get(), root);
    if ((v->tag == "Cell" || v->tag == "Dim") && v->args.size() == 1) {
      auto ref = compiler.forceValue(v->args.front());
      if (!ref || (*ref)->kind != Kind::Int || (*ref)->i < 0 ||
          static_cast<std::uint64_t>((*ref)->i) >
              std::numeric_limits<CellRef>::max() ||
          !builder.arena.contains(static_cast<CellRef>((*ref)->i)))
        throw std::runtime_error("Invalid Vlace cell capability in result");
      builder.link(root, builder.value, static_cast<CellRef>((*ref)->i));
      return root;
    }
    CellRef tail = root;
    for (const auto &arg : v->args) {
      auto record = builder.arena.makeCell();
      builder.link(tail, builder.arguments, record);
      builder.link(record, builder.value,
                   materializeLace(builder, compiler, arg, seen, depth + 1));
      tail = record;
    }
    return root;
  }
  default:
    throw std::runtime_error(
        "Function or native value is not a portable result");
  }
}
} // namespace

std::expected<std::string, std::string>
sealVortexProgram(const VortexProgram &program, SyntaxLimits limits) {
  try {
    validateProgram(program, limits);
    const auto body = programWire(program).encode();
    auto bytes =
        Wire::dict({{"format", Wire::string("xanadu-vortex-instructions")},
                    {"version", Wire::integer(1)},
                    {"body", Wire::string(body)},
                    {"sha256", Wire::string(xanadu::sha256Hex(body))}})
            .encode();
    (void)decodeProgram(bytes, limits);
    return bytes;
  } catch (const std::exception &error) {
    return std::unexpected(std::string(error.what()));
  }
}

std::expected<EvaluatedValue, std::string>
evaluate(Frontend frontend, std::string_view source, EvaluationBudget budget,
         const Manifold *snapshot, const xanadu::SpanReader *reader,
         SyntaxLimits syntax) {
  if (!budget.maxCycles || !budget.maxCells || !budget.maxSolutions ||
      !budget.maxOutputBytes || budget.maxCells >= ephemeralBit)
    return std::unexpected(
        "Evaluation budgets must be positive and representable");
  if (auto valid = checkSource(frontend, source, syntax); !valid)
    return std::unexpected(valid.error());
  try {
    auto context = std::make_unique<WorkerContext>(snapshot, reader);
    auto &arena  = context->arena;
    if (arena.cellCount() >= budget.maxCells)
      return std::unexpected(
          "Evaluation cell budget is smaller than runtime genesis");
    arena.setExecutionLimits(static_cast<std::uint32_t>(budget.maxCells),
                             budget.maxCycles, budget.maxOutputBytes);
    ResultBuilder builder{arena};
    auto root    = arena.makeCell("mesh:result");
    CellRef tail = root;
    switch (frontend) {
    case Frontend::Vpl: {
      vpl::VPLEngine engine(context->core);
      engine.withConsoleEffects(false);
      auto result = engine.evaluate(source);
      if (auto value = vplCell(builder, result, root))
        builder.append(tail, *value);
      break;
    }
    case Frontend::Vquery: {
      vql::VQLEngine engine(context->core);
      for (auto value : engine.execute(source)) builder.append(tail, value);
      break;
    }
    case Frontend::Vprolog: {
      vprolog::Parser parser(source);
      auto program = parser.parseProgram();
      vprolog::Compiler compiler(context->core);
      compiler.compileProgram(program);
      struct Binding {
        std::string name;
        LogicValue value;
      };
      std::vector<std::vector<Binding>> solutions;
      std::size_t capturedCells = 0;
      bool queried              = false;
      for (const auto &clause : program.clauses) {
        if (!clause.isQuery) continue;
        queried    = true;
        auto query = compiler.compileQuery(clause);
        compiler.stdlib().solve(
            query.goals,
            [&](const LogicSolution &solution) {
              if (solutions.size() >= budget.maxSolutions)
                throw std::runtime_error("Prolog solution budget exhausted");
              auto &row = solutions.emplace_back();
              for (const auto &[name, ref] : query.variables) {
                std::unordered_set<CellRef> active;
                const auto bound = solution.varMap.find(ref);
                row.push_back({name, captureLogic(compiler,
                                                  bound == solution.varMap.end()
                                                      ? ref
                                                      : bound->second,
                                                  active, capturedCells,
                                                  budget.maxCells)});
              }
              return true;
            },
            compiler.customPredicates(), budget.maxSolutions + 1);
      }
      if (!queried)
        throw std::runtime_error(
            "Prolog remote program requires an explicit query");
      for (const auto &row : solutions) {
        auto solution       = arena.makeCell("solution");
        CellRef bindingTail = solution;
        for (const auto &binding : row) {
          auto record = arena.makeCell(binding.name);
          builder.append(bindingTail, record);
          builder.link(record, builder.value,
                       materializeLogic(builder, binding.value));
        }
        builder.append(tail, solution);
      }
      break;
    }
    case Frontend::Vlace: {
      vlace::Parser parser(source);
      auto program = parser.parseProgram();
      if (!program) throw std::runtime_error(program.error().detail);
      vlace::TypeChecker checker;
      auto type = checker.checkProgram(*program);
      if (!type) throw std::runtime_error(type.error().detail);
      vlace::Compiler compiler(context->core, context->vm);
      auto value = compiler.evaluateProgram(*program);
      if (!value) throw std::runtime_error(value.error().detail);
      std::unordered_map<const vlace::Value *, CellRef> seen;
      builder.append(tail, materializeLace(builder, compiler, *value, seen));
      break;
    }
    case Frontend::Vortex: {
      auto artifactLimits   = syntax;
      artifactLimits.tokens = std::min(artifactLimits.tokens, budget.maxCells);
      auto program          = decodeProgram(source, artifactLimits);
      executeProgram(builder, *context, program, tail, budget);
      break;
    }
    }
    // Count the complete graph, including base spans and dimension labels,
    // before exposing an answer. Scratch limits alone do not cover quotations.
    std::unordered_set<CellRef> visited;
    std::vector<CellRef> pending;
    auto enqueue = [&](CellRef cell) {
      if (cell != noCell && visited.insert(cell).second) {
        if (visited.size() > budget.maxCells)
          throw std::runtime_error("Result graph cell budget exhausted");
        pending.push_back(cell);
      }
    };
    enqueue(root);
    std::size_t bytes = 0;
    while (!pending.empty()) {
      arena.consumeWork();
      auto cell = pending.back();
      pending.pop_back();
      for (const auto &span : arena.contentOf(cell)) {
        if (span.length > budget.maxOutputBytes - bytes)
          throw std::runtime_error("Result graph byte budget exhausted");
        bytes += static_cast<std::size_t>(span.length);
      }
      if (auto target = arena.handleTarget(cell)) enqueue(*target);
      for (const auto &edge : arena.dimensionsOf(cell)) {
        enqueue(edge.dim);
        enqueue(edge.pos);
        enqueue(edge.neg);
      }
    }
    return EvaluatedValue{.context = std::move(context), .root = root};
  } catch (const std::exception &error) {
    return std::unexpected(std::string(error.what()));
  }
}
} // namespace zigzag::vortex::mesh
