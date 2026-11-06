#include "common/xanadu/vql/compiler.hpp"

#include <limits>
#include <stdexcept>

#include "common/xanadu/vql/invocation.hpp"

namespace xanadu::vql {
using zigzag::CellRef;
using zigzag::DimRef;
using zigzag::vortex::OpcodeKind;

CellRef VQLCompiler::emitExistingTargetClone(const CellRef target) {
  const auto cloneTail =
      emitExtreme(target, core_.dims().clone, zigzag::DimVector::POS);
  const auto cloneOp = emitOp(OpcodeKind::New, "#CLONE_GENERATOR");
  emitInput(cloneOp, cloneTail);
  emitInputConstant(cloneOp, static_cast<std::int64_t>(core_.dims().clone));
  emitInputConstant(cloneOp, std::int64_t{1});

  const auto cloneRead = emitOp(OpcodeKind::Link, "#CLONE_RESULT");
  emitInput(cloneRead, cloneTail);
  emitInputConstant(cloneRead, static_cast<std::int64_t>(core_.dims().clone));
  emitInputConstant(cloneRead, std::int64_t{1});
  const CellRef cloneCell = core_.arena().makeCell();
  emitOutput(cloneRead, cloneCell);
  return cloneCell;
}

std::vector<CellRef> VQLCompiler::compileSelector(const PathStep &step,
                                                  CellRef current,
                                                  bool hasContext) {
  const std::vector<CellRef> stream{current};
  std::vector<CellRef> nextStream;
  if (std::holds_alternative<SignedDimensionStep>(step.selector)) {
    const auto &sd = std::get<SignedDimensionStep>(step.selector);
    DimRef dim     = resolveDimension(sd.dimName);

    if (!sd.creates.empty() &&
        (sd.placement != Placement::Default || sd.isBidir))
      throw std::invalid_argument("placed creation requires stream lowering");
    if (sd.creates.empty()) {
      if (sd.placement == Placement::Rank) {
        const auto head = emitExtreme(current, dim, zigzag::DimVector::NEG);
        nextStream.push_back(head);
        nextStream.push_back(emitRank(head, dim, zigzag::DimVector::POS));
        if (sd.direction == zigzag::DimVector::NEG)
          nextStream = {emitReverse(nextStream)};
      } else if (sd.placement == Placement::Head ||
                 sd.placement == Placement::Tail) {
        nextStream.push_back(emitExtreme(current, dim,
                                         sd.placement == Placement::Head
                                             ? zigzag::DimVector::NEG
                                             : zigzag::DimVector::POS));
      } else if (sd.isBidir) {
        nextStream.push_back(emitRank(current, dim, zigzag::DimVector::POS));
        nextStream.push_back(emitRank(current, dim, zigzag::DimVector::NEG));
      } else {
        nextStream.push_back(emitRank(current, dim, sd.direction));
      }
    } else {
      for (CellRef current : stream) {
        const CellRef group = beginMutationGroup("#CREATE_CELL_GROUP");
        std::vector<CellRef> createdCells;
        CellRef attachPoint = emitExtreme(current, dim, zigzag::DimVector::POS);
        for (const auto &createVal : sd.creates) {
          std::optional<CellRef> initialValue;
          if (createVal.kind == CreateValue::Kind::Expression && createVal.expr)
            initialValue = compileValueExpr(*createVal.expr, current);
          CellRef newOp = emitOp(OpcodeKind::New, "#MINT_CELL");
          emitInput(newOp, attachPoint);
          emitInputConstant(newOp, static_cast<std::int64_t>(dim));
          emitInputConstant(newOp, std::int64_t{1});

          // An output-less New mints on each execution. Read the resulting
          // neighbor into a reference register instead of reusing a graph cell.
          const auto read = emitOp(OpcodeKind::Link, "#MINT_RESULT");
          emitInput(read, attachPoint);
          emitInputConstant(read, static_cast<std::int64_t>(dim));
          emitInputConstant(read, std::int64_t{1});
          CellRef freshCell = core_.arena().makeCell();
          emitOutput(read, freshCell);

          if (createVal.kind == CreateValue::Kind::Literal) {
            CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
            emitInput(valOp, freshCell);
            emitInputConstant(valOp, static_cast<std::int64_t>(0));
            emitInputConstant(valOp, static_cast<std::int64_t>(-1));
            emitInputConstant(valOp, createVal.literal);
          } else if (createVal.kind == CreateValue::Kind::Expression &&
                     createVal.expr) {
            if (initialValue) {
              CellRef valOp = emitOp(OpcodeKind::Value, "#INIT_VALUE");
              emitInput(valOp, freshCell);
              emitInputConstant(valOp, static_cast<std::int64_t>(0));
              emitInputConstant(valOp, static_cast<std::int64_t>(-1));
              emitInput(valOp, *initialValue);
            }
          }
          createdCells.push_back(freshCell);
          attachPoint = freshCell;
        }
        endMutationGroup(group, "#CREATE_CELL_GROUP_END");

        nextStream.insert(nextStream.end(), createdCells.begin(),
                          createdCells.end());
      }
    }
  } else if (std::holds_alternative<FunctionInvocation>(step.selector)) {
    const auto &fn = std::get<FunctionInvocation>(step.selector);
    if (fn.name == "value") {
      for (CellRef current : stream) {
        if (fn.isSliceRead || fn.args.empty()) {
          std::optional<CellRef> offset;
          std::optional<CellRef> length;
          if (fn.args.size() >= 2) {
            offset = compileValueExpr(fn.args[1], current);
            if (!offset) continue;
          }
          if (fn.args.size() >= 3) {
            length = compileValueExpr(fn.args[2], current);
            if (!length) continue;
          }
          CellRef valOp = emitOp(OpcodeKind::Value, "#VALUE");
          emitInput(valOp, current);
          if (offset) emitInput(valOp, *offset);
          if (length) emitInput(valOp, *length);
          CellRef result = core_.arena().makeCell();
          emitOutput(valOp, result);
          nextStream.push_back(result);
        } else {
          const CellRef group    = beginMutationGroup("#VALUE_REPLACE_GROUP");
          const auto replacement = compileValueExpr(fn.args[0], current);
          std::optional<CellRef> offset;
          std::optional<CellRef> length;
          if (replacement && fn.args.size() >= 2)
            offset = compileValueExpr(fn.args[1], current);
          if (replacement && (fn.args.size() < 2 || offset) &&
              fn.args.size() >= 3)
            length = compileValueExpr(fn.args[2], current);
          if (replacement && (fn.args.size() < 2 || offset) &&
              (fn.args.size() < 3 || length)) {
            CellRef valOp = emitOp(OpcodeKind::Value, "#VALUE");
            emitInput(valOp, current);
            if (offset) {
              emitInput(valOp, *offset);
            } else {
              emitInputConstant(valOp, static_cast<std::int64_t>(0));
            }
            if (length) {
              emitInput(valOp, *length);
            } else {
              emitInputConstant(valOp,
                                std::numeric_limits<std::int64_t>::max());
            }
            emitInput(valOp, *replacement);
            nextStream.push_back(current);
          }
          endMutationGroup(group, "#VALUE_REPLACE_GROUP_END");
        }
      }
    } else if (fn.name == "link" && fn.args.size() >= 2) {
      // Existing-Target Fan-Out (§4.7)
      std::string dimName;
      zigzag::DimVector direction = zigzag::DimVector::POS;
      if (std::holds_alternative<ScalarLiteral>(fn.args[0].kind)) {
        const auto &lit = std::get<ScalarLiteral>(fn.args[0].kind);
        if (std::holds_alternative<std::string>(lit.value)) {
          std::string str = std::get<std::string>(lit.value);
          if (!str.empty() && str[0] == '-') {
            direction = zigzag::DimVector::NEG;
            dimName   = str.substr(1);
          } else if (!str.empty() && str[0] == '+') {
            direction = zigzag::DimVector::POS;
            dimName   = str.substr(1);
          } else {
            dimName = str;
          }
        }
      } else if (std::holds_alternative<std::shared_ptr<PathExpression>>(
                     fn.args[0].kind)) {
        const auto &p =
            *std::get<std::shared_ptr<PathExpression>>(fn.args[0].kind);
        if (!p.steps.empty() &&
            std::holds_alternative<SignedDimensionStep>(p.steps[0].selector)) {
          const auto &sd = std::get<SignedDimensionStep>(p.steps[0].selector);
          dimName        = sd.dimName;
          direction      = sd.direction;
        }
      }

      DimRef dim            = resolveDimension(dimName);
      const CellRef group   = beginMutationGroup("#CLONE_FANOUT_GROUP");
      const auto targetCell = compileValueExpr(fn.args[1], core_.home());

      std::vector<CellRef> attachedClones;
      if (targetCell) {
        for (CellRef current : stream) {
          const CellRef cloneCell = emitExistingTargetClone(*targetCell);

          CellRef linkOp = emitOp(OpcodeKind::Link, "#LINK_ATTACH " + dimName);
          emitInput(linkOp, current);
          emitInputConstant(linkOp, static_cast<std::int64_t>(dim));
          emitInputConstant(linkOp,
                            static_cast<std::int64_t>(toSign(direction)));
          emitInput(linkOp, cloneCell);
          attachedClones.push_back(cloneCell);
        }
      }

      nextStream = std::move(attachedClones);
      endMutationGroup(group, "#CLONE_FANOUT_GROUP_END");
    } else if (!definitions_.contains(fn.name) && fn.name == "string") {
      for (const auto current : stream) {
        const auto result = compileBuiltinInvocation(
            fn, hasContext ? std::optional{current} : std::nullopt);
        if (!result)
          throw std::logic_error("string builtin dispatch unexpectedly failed");
        nextStream.push_back(*result);
      }
    } else if (const auto definition = definitions_.find(fn.name);
               definition != definitions_.end()) {
      requirePositionalArguments(fn);
      for (const auto current : stream) {
        auto results = compileCall(
            definition->second, fn.args,
            hasContext ? std::optional{current} : std::nullopt, true);
        nextStream.insert(nextStream.end(), results.begin(), results.end());
      }
    } else {
      throw std::invalid_argument("VQL path function '" + fn.name +
                                  "' is not compiled to Vortex yet");
    }
  } else {
    throw std::invalid_argument(
        "this VQL path selector does not have Vortex graph lowering yet");
  }

  return nextStream;
}
} // namespace xanadu::vql
