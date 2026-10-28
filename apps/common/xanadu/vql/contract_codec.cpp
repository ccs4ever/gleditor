/**
 * @file contract_codec.cpp
 * @brief Portable, versioned encoding for VQL type contract descriptors.
 */
#include "common/xanadu/vql/contract_codec.hpp"

#include <charconv>
#include <cmath>
#include <format>
#include <iterator>
#include <limits>
#include <string>
#include <system_error>
#include <type_traits>

#include "common/xanadu/vql/parser.hpp"

namespace xanadu::vql {
namespace {

constexpr std::string_view descriptorHeader = "VQL_CONTRACT/1\n";

[[noreturn]] void unencodable(const std::string_view reason) {
  throw ContractDescriptorError(
      std::format("VQL contract descriptor cannot encode {}", reason));
}

void appendQuoted(std::string &out, const std::string_view value) {
  out.push_back('"');
  for (const char c : value) {
    switch (c) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\t':
      out += "\\t";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\b':
      out += "\\b";
      break;
    case '\f':
      out += "\\f";
      break;
    case '\0':
      out += "\\0";
      break;
    default:
      out.push_back(c);
      break;
    }
  }
  out.push_back('"');
}

void appendType(std::string &out, const TypeExpr &type);
void appendShape(std::string &out, const ShapeType &shape);
void appendDefault(std::string &out, const ValueExpr &value);

void appendField(std::string &out, const DestructureField &field) {
  if (field.isContext) {
    if (!field.dimName.empty() || field.type || field.defaultValue ||
        field.direction != zigzag::DimVector::POS || field.isBidir) {
      unencodable("a noncanonical context shape field");
    }
    out.push_back('.');
    if (!field.varName.empty()) {
      out += ": $";
      out += field.varName;
    }
    return;
  }
  if (field.dimName.empty()) {
    unencodable("a shape field with an empty dimension name");
  }
  if (field.isBidir) out.push_back('~');
  out += field.direction == zigzag::DimVector::NEG ? "-" : "+";
  out += field.dimName;
  if (!field.varName.empty()) {
    out += ": $";
    out += field.varName;
    if (field.type) {
      out += ": ";
      appendType(out, *field.type);
    }
  } else if (field.type) {
    out += ": ";
    appendType(out, *field.type);
  }
  if (field.defaultValue) {
    out += " := ";
    appendDefault(out, *field.defaultValue);
  }
}

void appendShape(std::string &out, const ShapeType &shape) {
  out.push_back('{');
  bool first = true;
  for (const auto &field : shape.pattern.fields) {
    if (!first) out += ", ";
    first = false;
    appendField(out, field);
  }
  out.push_back('}');
}

void appendType(std::string &out, const TypeExpr &type) {
  if (type.unionType.members.empty()) {
    unencodable("an empty union");
  }
  bool first = true;
  for (const auto &member : type.unionType.members) {
    if (!first) out += " | ";
    first = false;
    std::visit(
        [&](const auto &kind) {
          using T = std::decay_t<decltype(kind)>;
          if constexpr (std::is_same_v<T, BasicType>) {
            using K = BasicType::Kind;
            switch (kind.kind) {
            case K::Cell:
              out += "Cell";
              break;
            case K::Dim:
              out += "Dim";
              break;
            case K::Store:
              out += "Store";
              break;
            case K::Slice:
              out += "Slice";
              break;
            case K::Version:
              out += "Version";
              break;
            case K::Int:
              out += "Int";
              break;
            case K::Float:
              out += "Float";
              break;
            case K::String:
              out += "String";
              break;
            case K::Bool:
              out += "Bool";
              break;
            case K::Any:
              out += "Any";
              break;
            case K::Contextual:
              if (kind.name.empty()) unencodable("an empty contextual type");
              out += kind.name;
              break;
            }
          } else if constexpr (std::is_same_v<T, GenericType>) {
            if (kind.name.empty() || kind.args.empty()) {
              unencodable("an incomplete generic type");
            }
            out += kind.name;
            out.push_back('<');
            for (std::size_t i = 0; i < kind.args.size(); ++i) {
              if (i) out += ", ";
              if (!kind.args[i]) unencodable("a null generic type argument");
              appendType(out, *kind.args[i]);
            }
            out.push_back('>');
          } else if constexpr (std::is_same_v<T, ShapeType>) {
            appendShape(out, kind);
          } else {
            if (!kind) unencodable("a null parenthesized type");
            out.push_back('(');
            appendType(out, *kind);
            out.push_back(')');
          }
        },
        member.kind);
    if (member.refinement) appendShape(out, *member.refinement);
    if (member.isOptional) out.push_back('?');
  }
}

void appendPath(std::string &out, const PathExpression &path) {
  if (!path.anchorPredicates.empty() || path.shapeProjection ||
      path.cloneTail || path.anchor.versionPin || path.anchor.derefMaster ||
      (!path.isFunctor && path.anchor.kind != AnchorKind::Home &&
       path.anchor.kind != AnchorKind::Variable)) {
    unencodable("a path default outside the simple home/variable form");
  }
  if (!path.isFunctor) {
    if (path.anchor.kind == AnchorKind::Variable) {
      out.push_back('$');
      out += path.anchor.name;
    } else {
      out += "##";
    }
  }
  for (const auto &step : path.steps) {
    const auto *dimension = std::get_if<SignedDimensionStep>(&step.selector);
    if (!dimension || dimension->creates.size() ||
        dimension->placement != Placement::Default || step.derefMaster ||
        !step.predicates.empty() || !step.shapePredicates.empty() ||
        step.rangeClamp || step.versionPin ||
        step.yieldMode != YieldMode::Default) {
      unencodable("a path default outside simple dimension traversal");
    }
    out.push_back('/');
    out += dimension->direction == zigzag::DimVector::NEG ? "-" : "+";
    out += dimension->dimName;
  }
}

void appendDefault(std::string &out, const ValueExpr &value) {
  std::visit(
      [&](const auto &kind) {
        using T = std::decay_t<decltype(kind)>;
        if constexpr (std::is_same_v<T, ScalarLiteral>) {
          std::visit(
              [&](const auto &scalar) {
                using S = std::decay_t<decltype(scalar)>;
                if constexpr (std::is_same_v<S, std::string>) {
                  appendQuoted(out, scalar);
                } else if constexpr (std::is_same_v<S, bool>) {
                  out += scalar ? "true" : "false";
                } else if constexpr (std::is_same_v<S, std::int64_t>) {
                  out += std::to_string(scalar);
                } else {
                  if (!std::isfinite(scalar)) {
                    unencodable("a non-finite floating point default");
                  }
                  char buffer[128];
                  const auto result =
                      std::to_chars(std::begin(buffer), std::end(buffer),
                                    scalar, std::chars_format::general,
                                    std::numeric_limits<double>::max_digits10);
                  if (result.ec != std::errc{}) {
                    unencodable("a floating point default");
                  }
                  out.append(buffer, result.ptr);
                  if (std::string_view(
                          buffer, static_cast<std::size_t>(result.ptr - buffer))
                          .find_first_of(".eE") == std::string_view::npos) {
                    out += ".0";
                  }
                }
              },
              kind.value);
        } else if constexpr (std::is_same_v<T, std::string>) {
          out.push_back('$');
          out += kind;
        } else if constexpr (std::is_same_v<T,
                                            std::shared_ptr<PathExpression>>) {
          if (!kind) unencodable("a null path default");
          appendPath(out, *kind);
        } else {
          unencodable("a default expression other than a scalar, variable, or "
                      "simple path");
        }
      },
      value.kind);
}

} // namespace

std::string encodeTypeDescriptor(const TypeExpr &type) {
  std::string result(descriptorHeader);
  appendType(result, type);
  return result;
}

TypeExpr decodeTypeDescriptor(const std::string_view descriptor,
                              ContractDescriptorLimits limits) {
  if (descriptor.size() > limits.maxBytes)
    throw ContractDescriptorError("VQL contract descriptor exceeds byte limit");
  std::size_t nesting = 0;
  bool quoted         = false;
  bool escaped        = false;
  for (const auto byte : descriptor) {
    if (quoted) {
      if (escaped)
        escaped = false;
      else if (byte == '\\')
        escaped = true;
      else if (byte == '"')
        quoted = false;
    } else if (byte == '"')
      quoted = true;
    else if (byte == '(' || byte == '{' || byte == '<') {
      if (++nesting > limits.maxNesting)
        throw ContractDescriptorError(
            "VQL contract descriptor exceeds nesting limit");
    } else if (byte == ')' || byte == '}' || byte == '>') {
      if (nesting > 0) --nesting;
    }
  }
  if (!descriptor.starts_with("VQL_CONTRACT/")) {
    throw ContractDescriptorError(
        "VQL contract descriptor format mismatch: expected VQL_CONTRACT/1");
  }
  if (!descriptor.starts_with(descriptorHeader)) {
    const auto newline = descriptor.find('\n');
    const auto found   = descriptor.substr(
        0, newline == std::string_view::npos ? descriptor.size() : newline);
    throw ContractDescriptorError(
        std::format("VQL contract descriptor version mismatch: expected "
                    "VQL_CONTRACT/1, found {}",
                    found));
  }
  const auto syntax = descriptor.substr(descriptorHeader.size());
  if (syntax.empty()) {
    throw ContractDescriptorError(
        "VQL contract descriptor VQL_CONTRACT/1 has no type expression");
  }
  try {
    const std::string wrapped =
        std::format("def __contract($value: {}) := $value", syntax);
    const auto query = Parser(wrapped).parseQuery();
    if (query.defs.size() != 1 || query.defs.front().params.size() != 1 ||
        !query.defs.front().params.front().type ||
        !query.body.statements.empty() || !query.mounts.empty() ||
        !query.namespaceDeclarations.empty()) {
      throw ContractDescriptorError(
          "VQL contract descriptor did not contain exactly one type");
    }
    const auto *body =
        std::get_if<std::shared_ptr<ValueExpr>>(&query.defs.front().body);
    if (!body || !*body ||
        !std::holds_alternative<std::string>((*body)->kind) ||
        std::get<std::string>((*body)->kind) != "value")
      throw ContractDescriptorError(
          "VQL contract descriptor has trailing syntax");
    return *query.defs.front().params.front().type;
  } catch (const ContractDescriptorError &) {
    throw;
  } catch (const std::exception &error) {
    throw ContractDescriptorError(std::format(
        "malformed VQL_CONTRACT/1 type expression: {}", error.what()));
  }
}

} // namespace xanadu::vql
