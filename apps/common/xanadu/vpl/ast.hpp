/**
 * @file ast.hpp
 * @brief Abstract Syntax Tree (AST) definitions for Vortex Parallel Language
 * (VPL).
 *
 * Models APL/J array expressions, monadic and dyadic verbs, adverbs (/ \ ¨ ⌿),
 * conjunctions (∘. ⍤ ⌸), vector stranding, dimensions, and assignments.
 */
#ifndef COMMON_XANADU_VPL_AST_HPP
#define COMMON_XANADU_VPL_AST_HPP

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "common/xanadu/scanner_base.hpp"
#include "common/xanadu/vpl/token.hpp"

namespace xanadu::vpl {

class AstVisitor;

class AstNode {
public:
  explicit AstNode(SourceLocation loc) : location_(loc) {}
  virtual ~AstNode() = default;

  [[nodiscard]] SourceLocation location() const noexcept { return location_; }

  virtual void accept(AstVisitor &visitor) const = 0;

private:
  SourceLocation location_{};
};

class ScalarExpr final : public AstNode {
public:
  ScalarExpr(SourceLocation loc, double val, bool isFloat = false)
      : AstNode(loc), floatVal_(val), intVal_(static_cast<std::int64_t>(val)),
        isFloat_(isFloat) {}

  ScalarExpr(SourceLocation loc, std::int64_t val)
      : AstNode(loc), floatVal_(static_cast<double>(val)), intVal_(val),
        isFloat_(false) {}

  ScalarExpr(SourceLocation loc, std::string strVal)
      : AstNode(loc), stringVal_(std::move(strVal)), isString_(true) {}

  [[nodiscard]] bool isString() const noexcept { return isString_; }
  [[nodiscard]] bool isFloat() const noexcept { return isFloat_; }
  [[nodiscard]] double floatValue() const noexcept { return floatVal_; }
  [[nodiscard]] std::int64_t intValue() const noexcept { return intVal_; }
  [[nodiscard]] const std::string &stringValue() const noexcept {
    return stringVal_;
  }

  void accept(AstVisitor &visitor) const override;

private:
  double floatVal_{0.0};
  std::int64_t intVal_{0};
  std::string stringVal_{};
  bool isFloat_{false};
  bool isString_{false};
};

class VectorExpr final : public AstNode {
public:
  VectorExpr(SourceLocation loc, std::vector<std::shared_ptr<AstNode>> elements)
      : AstNode(loc), elements_(std::move(elements)) {}

  [[nodiscard]] const std::vector<std::shared_ptr<AstNode>> &
  elements() const noexcept {
    return elements_;
  }

  void accept(AstVisitor &visitor) const override;

private:
  std::vector<std::shared_ptr<AstNode>> elements_;
};

class DimensionExpr final : public AstNode {
public:
  DimensionExpr(SourceLocation loc, std::string name)
      : AstNode(loc), name_(std::move(name)) {}

  [[nodiscard]] const std::string &name() const noexcept { return name_; }

  void accept(AstVisitor &visitor) const override;

private:
  std::string name_;
};

class IdentifierExpr final : public AstNode {
public:
  IdentifierExpr(SourceLocation loc, std::string name)
      : AstNode(loc), name_(std::move(name)) {}

  [[nodiscard]] const std::string &name() const noexcept { return name_; }

  void accept(AstVisitor &visitor) const override;

private:
  std::string name_;
};

class VerbExpr final : public AstNode {
public:
  VerbExpr(SourceLocation loc, TokenKind verb) : AstNode(loc), verb_(verb) {}

  [[nodiscard]] TokenKind verb() const noexcept { return verb_; }

  void accept(AstVisitor &visitor) const override;

private:
  TokenKind verb_;
};

class MonadicExpr final : public AstNode {
public:
  MonadicExpr(SourceLocation loc, TokenKind verb,
              std::shared_ptr<AstNode> right)
      : AstNode(loc), verb_(verb), right_(std::move(right)) {}

  MonadicExpr(SourceLocation loc, std::shared_ptr<AstNode> customVerb,
              std::shared_ptr<AstNode> right)
      : AstNode(loc), customVerb_(std::move(customVerb)),
        right_(std::move(right)) {}

  [[nodiscard]] TokenKind verb() const noexcept { return verb_; }
  [[nodiscard]] const std::shared_ptr<AstNode> &customVerb() const noexcept {
    return customVerb_;
  }
  [[nodiscard]] bool hasCustomVerb() const noexcept {
    return customVerb_ != nullptr;
  }
  [[nodiscard]] const std::shared_ptr<AstNode> &right() const noexcept {
    return right_;
  }

  void accept(AstVisitor &visitor) const override;

private:
  TokenKind verb_{TokenKind::Error};
  std::shared_ptr<AstNode> customVerb_{nullptr};
  std::shared_ptr<AstNode> right_;
};

class DyadicExpr final : public AstNode {
public:
  DyadicExpr(SourceLocation loc, TokenKind verb, std::shared_ptr<AstNode> left,
             std::shared_ptr<AstNode> right)
      : AstNode(loc), verb_(verb), left_(std::move(left)),
        right_(std::move(right)) {}

  DyadicExpr(SourceLocation loc, std::shared_ptr<AstNode> customVerb,
             std::shared_ptr<AstNode> left, std::shared_ptr<AstNode> right)
      : AstNode(loc), customVerb_(std::move(customVerb)),
        left_(std::move(left)), right_(std::move(right)) {}

  [[nodiscard]] TokenKind verb() const noexcept { return verb_; }
  [[nodiscard]] const std::shared_ptr<AstNode> &customVerb() const noexcept {
    return customVerb_;
  }
  [[nodiscard]] bool hasCustomVerb() const noexcept {
    return customVerb_ != nullptr;
  }
  [[nodiscard]] const std::shared_ptr<AstNode> &left() const noexcept {
    return left_;
  }
  [[nodiscard]] const std::shared_ptr<AstNode> &right() const noexcept {
    return right_;
  }

  void accept(AstVisitor &visitor) const override;

private:
  TokenKind verb_{TokenKind::Error};
  std::shared_ptr<AstNode> customVerb_{nullptr};
  std::shared_ptr<AstNode> left_;
  std::shared_ptr<AstNode> right_;
};

class AdverbExpr final : public AstNode {
public:
  AdverbExpr(SourceLocation loc, TokenKind adverb,
             std::shared_ptr<AstNode> operand)
      : AstNode(loc), adverb_(adverb), operand_(std::move(operand)) {}

  [[nodiscard]] TokenKind adverb() const noexcept { return adverb_; }
  [[nodiscard]] const std::shared_ptr<AstNode> &operand() const noexcept {
    return operand_;
  }

  void accept(AstVisitor &visitor) const override;

private:
  TokenKind adverb_;
  std::shared_ptr<AstNode> operand_;
};

class ConjunctionExpr final : public AstNode {
public:
  ConjunctionExpr(SourceLocation loc, TokenKind conjunction,
                  std::shared_ptr<AstNode> left, std::shared_ptr<AstNode> right)
      : AstNode(loc), conjunction_(conjunction), left_(std::move(left)),
        right_(std::move(right)) {}

  [[nodiscard]] TokenKind conjunction() const noexcept { return conjunction_; }
  [[nodiscard]] const std::shared_ptr<AstNode> &left() const noexcept {
    return left_;
  }
  [[nodiscard]] const std::shared_ptr<AstNode> &right() const noexcept {
    return right_;
  }

  void accept(AstVisitor &visitor) const override;

private:
  TokenKind conjunction_;
  std::shared_ptr<AstNode> left_;
  std::shared_ptr<AstNode> right_;
};

class AssignExpr final : public AstNode {
public:
  AssignExpr(SourceLocation loc, std::string target,
             std::shared_ptr<AstNode> value)
      : AstNode(loc), target_(std::move(target)), value_(std::move(value)) {}

  [[nodiscard]] const std::string &target() const noexcept { return target_; }
  [[nodiscard]] const std::shared_ptr<AstNode> &value() const noexcept {
    return value_;
  }

  void accept(AstVisitor &visitor) const override;

private:
  std::string target_;
  std::shared_ptr<AstNode> value_;
};

class IndexingExpr final : public AstNode {
public:
  IndexingExpr(SourceLocation loc, std::shared_ptr<AstNode> target,
               std::vector<std::shared_ptr<AstNode>> indices)
      : AstNode(loc), target_(std::move(target)), indices_(std::move(indices)) {
  }

  [[nodiscard]] const std::shared_ptr<AstNode> &target() const noexcept {
    return target_;
  }
  [[nodiscard]] const std::vector<std::shared_ptr<AstNode>> &
  indices() const noexcept {
    return indices_;
  }

  void accept(AstVisitor &visitor) const override;

private:
  std::shared_ptr<AstNode> target_;
  std::vector<std::shared_ptr<AstNode>> indices_;
};

class QuadExpr final : public AstNode {
public:
  QuadExpr(SourceLocation loc, TokenKind quadKind,
           std::shared_ptr<AstNode> arg = nullptr)
      : AstNode(loc), quadKind_(quadKind), arg_(std::move(arg)) {}

  [[nodiscard]] TokenKind quadKind() const noexcept { return quadKind_; }
  [[nodiscard]] const std::shared_ptr<AstNode> &arg() const noexcept {
    return arg_;
  }

  void accept(AstVisitor &visitor) const override;

private:
  TokenKind quadKind_;
  std::shared_ptr<AstNode> arg_;
};

class Program final : public AstNode {
public:
  Program(SourceLocation loc, std::vector<std::shared_ptr<AstNode>> expressions)
      : AstNode(loc), expressions_(std::move(expressions)) {}

  [[nodiscard]] const std::vector<std::shared_ptr<AstNode>> &
  expressions() const noexcept {
    return expressions_;
  }

  void accept(AstVisitor &visitor) const override;

private:
  std::vector<std::shared_ptr<AstNode>> expressions_;
};

class AstVisitor {
public:
  virtual ~AstVisitor()                           = default;
  virtual void visit(const ScalarExpr &node)      = 0;
  virtual void visit(const VectorExpr &node)      = 0;
  virtual void visit(const DimensionExpr &node)   = 0;
  virtual void visit(const IdentifierExpr &node)  = 0;
  virtual void visit(const VerbExpr &node)        = 0;
  virtual void visit(const MonadicExpr &node)     = 0;
  virtual void visit(const DyadicExpr &node)      = 0;
  virtual void visit(const AdverbExpr &node)      = 0;
  virtual void visit(const ConjunctionExpr &node) = 0;
  virtual void visit(const AssignExpr &node)      = 0;
  virtual void visit(const IndexingExpr &node)    = 0;
  virtual void visit(const QuadExpr &node)        = 0;
  virtual void visit(const Program &node)         = 0;
};

inline void ScalarExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void VectorExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void DimensionExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void IdentifierExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void VerbExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void MonadicExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void DyadicExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void AdverbExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void ConjunctionExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void AssignExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void IndexingExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void QuadExpr::accept(AstVisitor &visitor) const {
  visitor.visit(*this);
}
inline void Program::accept(AstVisitor &visitor) const { visitor.visit(*this); }

} // namespace xanadu::vpl

#endif // COMMON_XANADU_VPL_AST_HPP
