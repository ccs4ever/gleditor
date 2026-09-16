/**
 * @file vpl_engine.cpp
 * @brief Direct AST evaluation engine implementation for VPL.
 */
#include "common/xanadu/vpl/vpl_engine.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iostream>
#include <numeric>
#include <sstream>

#include "common/xanadu/store.hpp"
#include "common/xanadu/vpl/lexer.hpp"

namespace xanadu::vpl {

namespace {

bool isScalarCell(const zigzag::ArenaManifold &arena, zigzag::CellRef c) {
  if (c == zigzag::noCell) return false;
  return arena.asDouble(c).has_value() || arena.asInt64(c).has_value() ||
         arena.asBool(c).has_value();
}

} // namespace

VPLEngine::VPLEngine() {
  ownedArena_ = std::make_unique<zigzag::ArenaManifold>();
  ownedCore_  = std::make_unique<zigzag::vortex::VortexCore>(*ownedArena_);
  ownedVm_    = std::make_unique<zigzag::vortex::VortexVM>(*ownedCore_);
  ownedStdlib_ =
      std::make_unique<zigzag::vortex::VortexStdLib>(*ownedCore_, *ownedVm_);

  core_   = ownedCore_.get();
  vm_     = ownedVm_.get();
  stdlib_ = ownedStdlib_.get();
}

VPLEngine::VPLEngine(zigzag::ArenaManifold &arena) {
  ownedCore_ = std::make_unique<zigzag::vortex::VortexCore>(arena);
  ownedVm_   = std::make_unique<zigzag::vortex::VortexVM>(*ownedCore_);
  ownedStdlib_ =
      std::make_unique<zigzag::vortex::VortexStdLib>(*ownedCore_, *ownedVm_);

  core_   = ownedCore_.get();
  vm_     = ownedVm_.get();
  stdlib_ = ownedStdlib_.get();
}

VPLEngine::VPLEngine(zigzag::vortex::VortexCore &core) : core_(&core) {
  ownedVm_ = std::make_unique<zigzag::vortex::VortexVM>(*core_);
  ownedStdlib_ =
      std::make_unique<zigzag::vortex::VortexStdLib>(*core_, *ownedVm_);

  vm_     = ownedVm_.get();
  stdlib_ = ownedStdlib_.get();
}

VPLEngine::VPLEngine(Store &store) : store_(&store) {
  ownedArena_ = std::make_unique<zigzag::ArenaManifold>();
  ownedCore_  = std::make_unique<zigzag::vortex::VortexCore>(*ownedArena_);
  ownedVm_    = std::make_unique<zigzag::vortex::VortexVM>(*ownedCore_);
  ownedStdlib_ =
      std::make_unique<zigzag::vortex::VortexStdLib>(*ownedCore_, *ownedVm_);

  core_   = ownedCore_.get();
  vm_     = ownedVm_.get();
  stdlib_ = ownedStdlib_.get();
}

VPLEngine::~VPLEngine() = default;

zigzag::DimRef VPLEngine::defaultDim() {
  if (defaultDim_ == zigzag::noCell) {
    defaultDim_ = resolveDimension("d.1");
  }
  return defaultDim_;
}

zigzag::DimRef VPLEngine::resolveDimension(std::string_view name) {
  const auto &sysDims = core_->dims();
  if (name == "d.stores") return sysDims.stores;
  if (name == "d.name") return sysDims.name;
  if (name == "d.role") return sysDims.role;
  if (name == "d.clone") return sysDims.clone;
  if (name == "d.dims") return sysDims.dims;
  if (name == "d.grab") return sysDims.grab;
  if (name == "d.step") return sysDims.step;
  if (name == "d.spin") return sysDims.spin;
  if (name == "d.stack") return sysDims.stack;
  if (name == "d.contract") return sysDims.contract;
  if (name == "d.cursors") return sysDims.cursors;
  if (name == "d.cache") return sysDims.cache;
  if (name == "d.vars") return sysDims.vars;
  if (name == "d.values") return sysDims.values;
  if (name == "d.pinning-cursors") return sysDims.pinningCursors;
  if (name == "d.stdlib") return sysDims.stdlib;
  if (name == "d.clause") return sysDims.clause;

  // Search existing dimensions on d.dims rank
  zigzag::CellRef curr = sysDims.dims;
  std::size_t limit    = core_->arena().cellCount() + 1;
  while (curr != zigzag::noCell && limit-- > 0) {
    if (core_->arena().textOf(curr) == name) {
      return curr;
    }
    curr = core_->arena().linked(curr, sysDims.dims, zigzag::DimVector::POS);
  }

  return core_->mintDimension(name);
}

void VPLEngine::setVariable(std::string_view name, VplView value) {
  env_[std::string(name)] = std::move(value);
}

std::optional<VplView> VPLEngine::getVariable(std::string_view name) const {
  auto it = env_.find(std::string(name));
  if (it != env_.end()) {
    return it->second;
  }
  return std::nullopt;
}

void VPLEngine::clearVariables() noexcept { env_.clear(); }

VplView VPLEngine::evaluate(std::string_view source) {
  Parser parser(source);
  auto program = parser.parseProgram();
  if (!program) {
    return VplView();
  }
  return evaluate(*program);
}

VplView VPLEngine::evaluate(const AstNode &node) {
  if (auto s = dynamic_cast<const ScalarExpr *>(&node)) {
    return evalScalar(*s);
  }
  if (auto v = dynamic_cast<const VectorExpr *>(&node)) {
    return evalVector(*v);
  }
  if (auto d = dynamic_cast<const DimensionExpr *>(&node)) {
    return evalDimension(*d);
  }
  if (auto id = dynamic_cast<const IdentifierExpr *>(&node)) {
    return evalIdentifier(*id);
  }
  if (auto m = dynamic_cast<const MonadicExpr *>(&node)) {
    return evalMonadic(*m);
  }
  if (auto dy = dynamic_cast<const DyadicExpr *>(&node)) {
    return evalDyadic(*dy);
  }
  if (auto a = dynamic_cast<const AdverbExpr *>(&node)) {
    return evalAdverb(*a);
  }
  if (auto c = dynamic_cast<const ConjunctionExpr *>(&node)) {
    return evalConjunction(*c);
  }
  if (auto as = dynamic_cast<const AssignExpr *>(&node)) {
    return evalAssign(*as);
  }
  if (auto idx = dynamic_cast<const IndexingExpr *>(&node)) {
    return evalIndexing(*idx);
  }
  if (auto q = dynamic_cast<const QuadExpr *>(&node)) {
    return evalQuad(*q);
  }
  if (auto p = dynamic_cast<const Program *>(&node)) {
    return evalProgram(*p);
  }
  return VplView();
}

VplView VPLEngine::evalScalar(const ScalarExpr &expr) {
  if (expr.isString()) {
    return VplView::makeScalar(expr.stringValue());
  }
  if (expr.isFloat()) {
    return VplView::makeScalar(expr.floatValue(), true);
  }
  return VplView::makeScalar(expr.intValue());
}

VplView VPLEngine::evalVector(const VectorExpr &expr) {
  if (expr.elements().empty()) {
    return VplView();
  }

  std::vector<VplView> elemViews;
  elemViews.reserve(expr.elements().size());
  bool allScalars    = true;
  bool anyFloat      = false;
  bool anyString     = false;
  bool allDimensions = true;

  for (const auto &el : expr.elements()) {
    VplView v = evaluate(*el);
    if (!v.isScalar()) {
      allScalars = false;
    } else {
      if (v.isFloat()) anyFloat = true;
      if (v.isString()) anyString = true;
    }
    if (v.valence() != 1 || v.origin() != zigzag::noCell) {
      allDimensions = false;
    }
    elemViews.push_back(std::move(v));
  }

  // Vector of dimensions (e.g. (d.1 d.2 d.3))
  if (allDimensions && !elemViews.empty()) {
    std::vector<zigzag::DirectedDim> axes;
    axes.reserve(elemViews.size());
    for (const auto &ev : elemViews) {
      if (!ev.axes().empty()) {
        axes.push_back(ev.axes()[0]);
      }
    }
    return VplView(zigzag::noCell, std::move(axes));
  }

  // All scalar strings
  if (allScalars && anyString) {
    std::vector<std::string> strVals;
    strVals.reserve(elemViews.size());
    for (const auto &ev : elemViews) {
      strVals.push_back(ev.isString() ? ev.scalarString()
                                      : std::to_string(ev.scalarInt()));
    }
    return mintRank(strVals, defaultDim());
  }

  // All numeric scalars
  if (allScalars) {
    if (anyFloat) {
      std::vector<double> vals;
      vals.reserve(elemViews.size());
      for (const auto &ev : elemViews) {
        vals.push_back(ev.isFloat() ? ev.scalarFloat()
                                    : static_cast<double>(ev.scalarInt()));
      }
      return mintRank(vals, defaultDim());
    } else {
      std::vector<std::int64_t> vals;
      vals.reserve(elemViews.size());
      for (const auto &ev : elemViews) {
        vals.push_back(ev.scalarInt());
      }
      return mintRank(vals, defaultDim());
    }
  }

  // Mixed or sub-views: mint cells and link
  std::vector<zigzag::CellRef> cells;
  for (const auto &ev : elemViews) {
    if (ev.origin() != zigzag::noCell) {
      cells.push_back(ev.origin());
    } else if (ev.isScalar()) {
      zigzag::CellRef c;
      if (ev.isString()) {
        c = arena().makeCell(ev.scalarString());
      } else if (ev.isFloat()) {
        c = arena().makeScalarCell(ev.scalarFloat());
      } else {
        c = arena().makeScalarCell(ev.scalarInt());
      }
      cells.push_back(c);
    }
  }

  if (cells.empty()) return VplView();

  zigzag::DimRef axis = defaultDim();
  for (std::size_t i = 1; i < cells.size(); ++i) {
    arena().link(cells[i - 1], axis, zigzag::DimVector::POS, cells[i]);
  }
  VplView res(cells[0], {zigzag::DirectedDim{axis, zigzag::DimVector::POS}});
  res.setExtents({cells.size()});
  return res;
}

VplView VPLEngine::evalDimension(const DimensionExpr &expr) {
  zigzag::DimRef dim = resolveDimension(expr.name());
  return VplView(zigzag::noCell,
                 {zigzag::DirectedDim{dim, zigzag::DimVector::POS}});
}

VplView VPLEngine::evalIdentifier(const IdentifierExpr &expr) {
  if (expr.name() == "H") {
    return VplView(core_->home(), std::vector<zigzag::DirectedDim>{});
  }
  if (expr.name().starts_with("d.")) {
    zigzag::DimRef dim = resolveDimension(expr.name());
    return VplView(zigzag::noCell,
                   {zigzag::DirectedDim{dim, zigzag::DimVector::POS}});
  }
  auto it = env_.find(expr.name());
  if (it != env_.end()) {
    return it->second;
  }
  return VplView();
}

VplView VPLEngine::evalAssign(const AssignExpr &expr) {
  VplView val         = evaluate(*expr.value());
  env_[expr.target()] = val;
  return val;
}

VplView VPLEngine::evalIndexing(const IndexingExpr &expr) {
  VplView target = evaluate(*expr.target());
  if (expr.indices().empty()) {
    return target;
  }

  VplView idxView = evaluate(*expr.indices()[0]);
  auto cells      = target.collectCells(arena());
  if (cells.empty()) {
    return VplView();
  }

  // Single scalar integer index (e.g. A[3])
  if (idxView.isScalar()) {
    std::int64_t k = idxView.scalarInt();
    if (k < 0) k += static_cast<std::int64_t>(cells.size());
    if (k >= 0 && static_cast<std::size_t>(k) < cells.size()) {
      zigzag::CellRef c = cells[k];
      if (isScalarCell(arena(), c)) {
        if (auto d = arena().asDouble(c); d.has_value()) {
          return VplView::makeScalar(*d, true, c);
        }
        if (auto n = arena().asInt64(c); n.has_value()) {
          return VplView::makeScalar(*n, c);
        }
      }
      return VplView::makeScalar(arena().textOf(c), c);
    }
    return VplView();
  }

  // Vector index (e.g. W[⍋≢¨W])
  auto idxCells = idxView.collectCells(arena());
  std::vector<std::int64_t> indices;
  for (zigzag::CellRef ic : idxCells) {
    indices.push_back(cellValueInt(ic));
  }

  std::vector<zigzag::CellRef> selected;
  selected.reserve(indices.size());
  for (std::int64_t idx : indices) {
    if (idx >= 0 && static_cast<std::size_t>(idx) < cells.size()) {
      selected.push_back(cells[idx]);
    }
  }

  if (selected.empty()) return VplView();

  zigzag::DimRef axis  = defaultDim();
  zigzag::CellRef head = zigzag::noCell;
  zigzag::CellRef prev = zigzag::noCell;
  for (zigzag::CellRef c : selected) {
    zigzag::CellRef copy = arena().makeCell();
    if (isScalarCell(arena(), c)) {
      if (auto d = arena().asDouble(c); d.has_value()) {
        copy = arena().makeScalarCell(*d);
      } else if (auto n = arena().asInt64(c); n.has_value()) {
        copy = arena().makeScalarCell(*n);
      }
    } else {
      core_->value(copy, 0, -1, core_->render(c));
    }
    if (head == zigzag::noCell) {
      head = copy;
    } else {
      arena().link(prev, axis, zigzag::DimVector::POS, copy);
    }
    prev = copy;
  }
  VplView res(head, {zigzag::DirectedDim{axis, zigzag::DimVector::POS}});
  res.setExtents({selected.size()});
  return res;
}

VplView VPLEngine::evalProgram(const Program &expr) {
  VplView last;
  for (const auto &e : expr.expressions()) {
    last = evaluate(*e);
  }
  return last;
}

VplView VPLEngine::evalMonadic(const MonadicExpr &expr) {
  VplView right = evaluate(*expr.right());

  if (expr.hasCustomVerb()) {
    if (auto adv = dynamic_cast<const AdverbExpr *>(expr.customVerb().get())) {
      TokenKind adverb = adv->adverb();
      TokenKind verb   = TokenKind::Plus;
      if (auto v = dynamic_cast<const VerbExpr *>(adv->operand().get())) {
        verb = v->verb();
      }

      // Reduce (/): fold along walk order
      if (adverb == TokenKind::Reduce) {
        auto cells = right.collectCells(arena());
        if (cells.empty()) {
          if (right.isScalar()) return right;
          return VplView::makeScalar(computeDyadicScalar(verb, 0.0, 0.0));
        }
        double acc = cellValueDouble(cells[0]);
        for (std::size_t i = 1; i < cells.size(); ++i) {
          acc = computeDyadicScalar(verb, acc, cellValueDouble(cells[i]));
        }
        return VplView::makeScalar(static_cast<std::int64_t>(acc));
      }

      // Scan (\): fold emitting intermediate cells
      if (adverb == TokenKind::Scan) {
        auto cells = right.collectCells(arena());
        if (cells.empty()) {
          return right;
        }
        std::vector<std::int64_t> partials;
        double acc = cellValueDouble(cells[0]);
        partials.push_back(static_cast<std::int64_t>(acc));
        for (std::size_t i = 1; i < cells.size(); ++i) {
          acc = computeDyadicScalar(verb, acc, cellValueDouble(cells[i]));
          partials.push_back(static_cast<std::int64_t>(acc));
        }
        zigzag::DimRef axis =
            right.axes().empty() ? defaultDim() : right.axes()[0].dim;
        return mintRank(partials, axis);
      }

      // Each (¨): apply per-cell
      if (adverb == TokenKind::Each) {
        auto cells = right.collectCells(arena());
        std::vector<std::int64_t> results;
        for (zigzag::CellRef c : cells) {
          VplView cellView = VplView::makeScalar(cellValueDouble(c), false, c);
          VplView mapped   = applyMonadicVerb(verb, cellView);
          results.push_back(mapped.scalarInt());
        }
        zigzag::DimRef axis =
            right.axes().empty() ? defaultDim() : right.axes()[0].dim;
        return mintRank(results, axis);
      }
    }
  }

  return applyMonadicVerb(expr.verb(), right);
}

VplView VPLEngine::evalDyadic(const DyadicExpr &expr) {
  VplView left  = evaluate(*expr.left());
  VplView right = evaluate(*expr.right());

  if (expr.hasCustomVerb()) {
    // Outer Product: left ∘.verb right
    if (auto conj =
            dynamic_cast<const ConjunctionExpr *>(expr.customVerb().get())) {
      if (conj->conjunction() == TokenKind::OuterProduct) {
        TokenKind verb = TokenKind::Plus;
        if (auto v = dynamic_cast<const VerbExpr *>(conj->left().get())) {
          verb = v->verb();
        }

        auto leftCells  = left.collectCells(arena());
        auto rightCells = right.collectCells(arena());
        if (leftCells.empty() || rightCells.empty()) {
          return VplView();
        }

        zigzag::DimRef dRow = defaultDim();
        zigzag::DimRef dCol = resolveDimension("d.2");

        zigzag::CellRef root        = zigzag::noCell;
        zigzag::CellRef prevRowHead = zigzag::noCell;

        for (zigzag::CellRef lc : leftCells) {
          double lVal             = cellValueDouble(lc);
          zigzag::CellRef rowHead = zigzag::noCell;
          zigzag::CellRef prevCol = zigzag::noCell;

          for (zigzag::CellRef rc : rightCells) {
            double rVal = cellValueDouble(rc);
            double val  = computeDyadicScalar(verb, lVal, rVal);
            zigzag::CellRef cell =
                arena().makeScalarCell(static_cast<std::int64_t>(val));

            if (rowHead == zigzag::noCell) rowHead = cell;
            if (prevCol != zigzag::noCell) {
              arena().link(prevCol, dCol, zigzag::DimVector::POS, cell);
            }
            prevCol = cell;
          }

          if (root == zigzag::noCell) root = rowHead;
          if (prevRowHead != zigzag::noCell) {
            arena().link(prevRowHead, dRow, zigzag::DimVector::POS, rowHead);
          }
          prevRowHead = rowHead;
        }

        VplView res(root, {zigzag::DirectedDim{dRow, zigzag::DimVector::POS},
                           zigzag::DirectedDim{dCol, zigzag::DimVector::POS}});
        res.setExtents({leftCells.size(), rightCells.size()});
        return res;
      }
    }

    // Compress: mask ⌿ view
    if (auto adv = dynamic_cast<const AdverbExpr *>(expr.customVerb().get())) {
      if (adv->adverb() == TokenKind::Compress) {
        auto maskCells   = left.collectCells(arena());
        auto targetCells = right.collectCells(arena());
        std::vector<zigzag::CellRef> selected;
        for (std::size_t i = 0;
             i < std::min(maskCells.size(), targetCells.size()); ++i) {
          if (cellValueDouble(maskCells[i]) != 0.0) {
            selected.push_back(targetCells[i]);
          }
        }
        zigzag::DimRef axis =
            right.axes().empty() ? defaultDim() : right.axes()[0].dim;
        zigzag::CellRef head = zigzag::noCell;
        zigzag::CellRef prev = zigzag::noCell;
        for (zigzag::CellRef sc : selected) {
          zigzag::CellRef copy = arena().makeCell();
          if (isScalarCell(arena(), sc)) {
            if (auto n = arena().asInt64(sc); n.has_value()) {
              copy = arena().makeScalarCell(*n);
            } else if (auto d = arena().asDouble(sc); d.has_value()) {
              copy = arena().makeScalarCell(*d);
            }
          } else {
            core_->value(copy, 0, -1, core_->render(sc));
          }
          if (head == zigzag::noCell) head = copy;
          if (prev != zigzag::noCell) {
            arena().link(prev, axis, zigzag::DimVector::POS, copy);
          }
          prev = copy;
        }
        VplView res(head, {zigzag::DirectedDim{axis, zigzag::DimVector::POS}});
        res.setExtents({selected.size()});
        return res;
      }
    }
  }

  return applyDyadicVerb(expr.verb(), left, right);
}

VplView VPLEngine::evalAdverb(const AdverbExpr &expr) {
  VplView operand = evaluate(*expr.operand());
  return operand;
}

VplView VPLEngine::evalConjunction(const ConjunctionExpr &expr) {
  // Key Operator (⌸): group rank by key function into clone ranks
  if (expr.conjunction() == TokenKind::Key) {
    VplView data   = evaluate(*expr.left());
    auto dataCells = data.collectCells(arena());

    std::unordered_map<std::string, std::vector<zigzag::CellRef>> groups;
    for (zigzag::CellRef dc : dataCells) {
      std::string k = arena().textOf(dc);
      if (k.empty()) k = std::to_string(cellValueInt(dc));
      groups[k].push_back(dc);
    }

    zigzag::DimRef dClone = core_->dims().clone;
    std::vector<std::string> keys;
    for (const auto &[k, members] : groups) {
      keys.push_back(k);
      zigzag::CellRef master = arena().makeCell(k);
      zigzag::CellRef prev   = master;
      for (zigzag::CellRef m : members) {
        (void)m;
        zigzag::CellRef cloneCell = arena().makeCell();
        arena().link(prev, dClone, zigzag::DimVector::POS, cloneCell);
        prev = cloneCell;
      }
    }
    return mintRank(keys, defaultDim());
  }

  return VplView();
}

VplView VPLEngine::evalQuad(const QuadExpr &expr) {
  // ⎕ (Print)
  if (expr.quadKind() == TokenKind::Quad) {
    if (expr.arg()) {
      VplView v = evaluate(*expr.arg());
      if (v.isScalar()) {
        if (v.isString()) {
          std::cout << v.scalarString() << "\n";
        } else if (v.isFloat()) {
          std::cout << v.scalarFloat() << "\n";
        } else {
          std::cout << v.scalarInt() << "\n";
        }
      } else {
        auto cells = v.collectCells(arena());
        for (std::size_t i = 0; i < cells.size(); ++i) {
          if (i > 0) std::cout << " ";
          if (isScalarCell(arena(), cells[i])) {
            std::cout << cellValueInt(cells[i]);
          } else {
            std::cout << arena().textOf(cells[i]);
          }
        }
        std::cout << "\n";
      }
      return v;
    }
    return VplView();
  }

  // ⎕READ
  if (expr.quadKind() == TokenKind::QuadRead) {
    // Mint words or content
    std::vector<std::string> words = {"document", "stream", "chapter",
                                      "xanadu"};
    zigzag::DimRef docDim          = resolveDimension("d.doc");
    return mintRank(words, docDim);
  }

  // ⎕SPLIT
  if (expr.quadKind() == TokenKind::QuadSplit) {
    std::string s;
    if (expr.arg()) {
      VplView v = evaluate(*expr.arg());
      s         = v.scalarString();
    }
    std::vector<std::string> tokens =
        zigzag::vortex::VortexStdLib::strSplit(s, " ");
    return mintRank(tokens, defaultDim());
  }

  return VplView();
}

VplView VPLEngine::applyMonadicVerb(TokenKind verb, const VplView &arg) {
  // ⍳ (Iota): mint rank of n cells along default axis
  if (verb == TokenKind::Iota) {
    std::size_t n =
        static_cast<std::size_t>(std::max<std::int64_t>(0, arg.scalarInt()));
    zigzag::DimRef dim   = defaultDim();
    zigzag::CellRef head = stdlib_->arrayIota(n, dim);
    VplView res(head, {zigzag::DirectedDim{dim, zigzag::DimVector::POS}});
    res.setExtents({n});
    return res;
  }

  // ⍴ (Shape / Rho)
  if (verb == TokenKind::Rho) {
    if (arg.isScalar()) {
      return VplView::makeScalar(static_cast<std::int64_t>(0));
    }
    auto sh = arg.shape(arena());
    std::vector<std::int64_t> shInt(sh.begin(), sh.end());
    return mintRank(shInt, defaultDim());
  }

  // ≢ (Tally)
  if (verb == TokenKind::Tally) {
    if (arg.isScalar()) {
      if (arg.isString()) {
        return VplView::makeScalar(
            static_cast<std::int64_t>(arg.scalarString().size()));
      }
      return VplView::makeScalar(static_cast<std::int64_t>(1));
    }
    auto sh = arg.shape(arena());
    if (sh.empty()) return VplView::makeScalar(static_cast<std::int64_t>(0));
    if (sh.size() == 1 && sh[0] == 1 && arg.origin() != zigzag::noCell) {
      std::string txt = arena().textOf(arg.origin());
      if (!txt.empty()) {
        return VplView::makeScalar(static_cast<std::int64_t>(txt.size()));
      }
    }
    return VplView::makeScalar(static_cast<std::int64_t>(sh[0]));
  }

  // ⍉ (Transpose)
  if (verb == TokenKind::Transpose) {
    VplView copy = arg;
    copy.transpose();
    return copy;
  }

  // ⌽ (Reverse first axis)
  if (verb == TokenKind::ReverseFirst) {
    VplView copy = arg;
    copy.reverseFirst();
    return copy;
  }

  // ⊖ (Reverse last axis)
  if (verb == TokenKind::ReverseLast) {
    VplView copy = arg;
    copy.reverseLast();
    return copy;
  }

  // ⊂ (Enclose)
  if (verb == TokenKind::Enclose) {
    VplView res;
    res.enclose(std::make_shared<VplView>(arg));
    return res;
  }

  // ⊃ (Disclose / Master)
  if (verb == TokenKind::Disclose) {
    if (arg.isEnclosed() && arg.enclosedView()) {
      return *arg.enclosedView();
    }
    if (arg.origin() != zigzag::noCell) {
      zigzag::CellRef master =
          arena().cloneMaster(arg.origin(), core_->dims().clone);
      if (master != arg.origin() && master != zigzag::noCell) {
        return VplView(master, arg.axes());
      }
      auto cells = arg.collectCells(arena());
      if (!cells.empty()) {
        return VplView(cells[0], std::vector<zigzag::DirectedDim>{});
      }
    }
    return arg;
  }

  // ≡ (Depth / Valence)
  if (verb == TokenKind::Match || verb == TokenKind::Equal) {
    return VplView::makeScalar(static_cast<std::int64_t>(arg.valence()));
  }

  // ⍸ (Where)
  if (verb == TokenKind::Where) {
    auto cells = arg.collectCells(arena());
    std::vector<std::int64_t> indices;
    for (std::size_t i = 0; i < cells.size(); ++i) {
      if (cellValueDouble(cells[i]) != 0.0) {
        indices.push_back(static_cast<std::int64_t>(i));
      }
    }
    return mintRank(indices, defaultDim());
  }

  // ⍋ (Grade up)
  if (verb == TokenKind::GradeUp) {
    auto cells = arg.collectCells(arena());
    std::vector<std::size_t> indices(cells.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::stable_sort(
        indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
          return cellValueDouble(cells[a]) < cellValueDouble(cells[b]);
        });
    std::vector<std::int64_t> idx64(indices.begin(), indices.end());
    return mintRank(idx64, defaultDim());
  }

  // ⍒ (Grade down)
  if (verb == TokenKind::GradeDown) {
    auto cells = arg.collectCells(arena());
    std::vector<std::size_t> indices(cells.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::stable_sort(
        indices.begin(), indices.end(), [&](std::size_t a, std::size_t b) {
          return cellValueDouble(cells[a]) > cellValueDouble(cells[b]);
        });
    std::vector<std::int64_t> idx64(indices.begin(), indices.end());
    return mintRank(idx64, defaultDim());
  }

  // ⍟ (Hypertime)
  if (verb == TokenKind::Hypertime) {
    return VplView::makeScalar(static_cast<std::int64_t>(arena().cellCount()));
  }

  // ⌺ (Transclusions)
  if (verb == TokenKind::Transclude) {
    auto cells = arg.collectCells(arena());
    std::vector<zigzag::CellRef> trans;
    zigzag::DimRef dTrans = resolveDimension("d.transclude");
    for (zigzag::CellRef c : cells) {
      zigzag::CellRef t = arena().linked(c, dTrans, zigzag::DimVector::POS);
      std::size_t limit = arena().cellCount() + 1;
      while (t != zigzag::noCell && limit-- > 0) {
        trans.push_back(t);
        t = arena().linked(t, dTrans, zigzag::DimVector::POS);
      }
    }
    if (trans.empty()) return VplView();
    VplView res(trans[0],
                {zigzag::DirectedDim{dTrans, zigzag::DimVector::POS}});
    res.setExtents({trans.size()});
    return res;
  }

  // Monadic math & logic (+, -, ×, ÷, |, ~, ⌊, ⌈)
  if (arg.isScalar()) {
    double val = arg.isFloat() ? arg.scalarFloat()
                               : static_cast<double>(arg.scalarInt());
    double res = computeMonadicScalar(verb, val);
    return VplView::makeScalar(res, arg.isFloat());
  }

  auto cells = arg.collectCells(arena());
  std::vector<double> vals;
  for (zigzag::CellRef c : cells) {
    vals.push_back(computeMonadicScalar(verb, cellValueDouble(c)));
  }
  return mintRank(vals, arg.axes().empty() ? defaultDim() : arg.axes()[0].dim);
}

VplView VPLEngine::applyDyadicVerb(TokenKind verb, const VplView &left,
                                   const VplView &right) {
  // 5 ⍳ d.1 : mint 5 cells on d.1 rank from H
  if (verb == TokenKind::Iota) {
    std::size_t n =
        static_cast<std::size_t>(std::max<std::int64_t>(0, left.scalarInt()));
    zigzag::DimRef d =
        right.axes().empty() ? defaultDim() : right.axes()[0].dim;

    zigzag::CellRef endOfH =
        arena().linked(core_->home(), d, zigzag::DimVector::POS);
    zigzag::CellRef lastH = core_->home();
    std::size_t limit     = arena().cellCount() + 1;
    while (endOfH != zigzag::noCell && limit-- > 0) {
      lastH  = endOfH;
      endOfH = arena().linked(endOfH, d, zigzag::DimVector::POS);
    }

    zigzag::CellRef firstMinted = zigzag::noCell;
    zigzag::CellRef prev        = lastH;
    for (std::size_t i = 1; i <= n; ++i) {
      zigzag::CellRef c = arena().makeScalarCell(static_cast<std::int64_t>(i));
      if (firstMinted == zigzag::noCell) firstMinted = c;
      arena().link(prev, d, zigzag::DimVector::POS, c);
      prev = c;
    }

    VplView res(firstMinted, {zigzag::DirectedDim{d, zigzag::DimVector::POS}});
    res.setExtents({n});
    return res;
  }

  // ⍴ (Reshape / View construction)
  if (verb == TokenKind::Rho) {
    // Case 1: H ⍴ (d.1 d.2 d.3)
    if (left.origin() != zigzag::noCell && !right.axes().empty()) {
      VplView res(left.origin(), right.axes());
      res.setExtents(res.shape(arena()));
      return res;
    }

    // Case 2: 1 2 3 4 5 ⍴ d.1
    if (!right.axes().empty() && left.valence() > 0) {
      zigzag::DimRef d = right.axes()[0].dim;
      auto cells       = left.collectCells(arena());
      for (std::size_t i = 1; i < cells.size(); ++i) {
        arena().link(cells[i - 1], d, zigzag::DimVector::POS, cells[i]);
      }
      VplView res(cells.empty() ? zigzag::noCell : cells[0],
                  {zigzag::DirectedDim{d, zigzag::DimVector::POS}});
      res.setExtents({cells.size()});
      return res;
    }

    // Case 3: (rows cols) ⍴ items (Reshape into 2D)
    auto shapeCells = left.collectCells(arena());
    std::vector<std::size_t> newDims;
    if (left.isScalar()) {
      newDims.push_back(static_cast<std::size_t>(left.scalarInt()));
    } else {
      for (zigzag::CellRef sc : shapeCells) {
        newDims.push_back(static_cast<std::size_t>(cellValueInt(sc)));
      }
    }

    auto itemCells = right.collectCells(arena());
    if (newDims.size() >= 2 && !itemCells.empty()) {
      zigzag::DimRef dRow = defaultDim();
      zigzag::DimRef dCol = resolveDimension("d.2");
      std::size_t R       = newDims[0];
      std::size_t C       = newDims[1];

      zigzag::CellRef root        = zigzag::noCell;
      zigzag::CellRef prevRowHead = zigzag::noCell;

      for (std::size_t r = 0; r < R; ++r) {
        zigzag::CellRef rowHead = zigzag::noCell;
        zigzag::CellRef prevCol = zigzag::noCell;

        for (std::size_t c = 0; c < C; ++c) {
          std::size_t idx      = (r * C + c) % itemCells.size();
          zigzag::CellRef cell = arena().makeCell();
          if (isScalarCell(arena(), itemCells[idx])) {
            if (auto d = arena().asDouble(itemCells[idx]); d.has_value()) {
              cell = arena().makeScalarCell(*d);
            } else if (auto n = arena().asInt64(itemCells[idx]);
                       n.has_value()) {
              cell = arena().makeScalarCell(*n);
            }
          } else {
            core_->value(cell, 0, -1, core_->render(itemCells[idx]));
          }
          if (rowHead == zigzag::noCell) rowHead = cell;
          if (prevCol != zigzag::noCell) {
            arena().link(prevCol, dCol, zigzag::DimVector::POS, cell);
          }
          prevCol = cell;
        }

        if (root == zigzag::noCell) root = rowHead;
        if (prevRowHead != zigzag::noCell) {
          arena().link(prevRowHead, dRow, zigzag::DimVector::POS, rowHead);
        }
        prevRowHead = rowHead;
      }

      VplView res(root, {zigzag::DirectedDim{dRow, zigzag::DimVector::POS},
                         zigzag::DirectedDim{dCol, zigzag::DimVector::POS}});
      res.setExtents({R, C});
      return res;
    }
    return right;
  }

  // ⍉ (Dyadic Transpose with permutation)
  if (verb == TokenKind::Transpose) {
    auto pCells = left.collectCells(arena());
    std::vector<std::size_t> perm;
    for (zigzag::CellRef pc : pCells) {
      perm.push_back(static_cast<std::size_t>(cellValueInt(pc)));
    }
    VplView copy = right;
    copy.transpose(perm);
    return copy;
  }

  // ↑ (Take)
  if (verb == TokenKind::Take) {
    std::int64_t n = left.scalarInt();
    auto cells     = right.collectCells(arena());
    if (cells.empty()) return VplView();
    std::vector<zigzag::CellRef> taken;
    if (n >= 0) {
      std::size_t count = std::min(static_cast<std::size_t>(n), cells.size());
      taken.assign(cells.begin(), cells.begin() + count);
    } else {
      std::size_t count = std::min(static_cast<std::size_t>(-n), cells.size());
      taken.assign(cells.end() - count, cells.end());
    }
    zigzag::DimRef axis =
        right.axes().empty() ? defaultDim() : right.axes()[0].dim;
    for (std::size_t i = 1; i < taken.size(); ++i) {
      arena().link(taken[i - 1], axis, zigzag::DimVector::POS, taken[i]);
    }
    VplView res(taken.empty() ? zigzag::noCell : taken[0],
                {zigzag::DirectedDim{axis, zigzag::DimVector::POS}});
    res.setExtents({taken.size()});
    return res;
  }

  // ↓ (Drop)
  if (verb == TokenKind::Drop) {
    std::int64_t n = left.scalarInt();
    auto cells     = right.collectCells(arena());
    if (cells.empty()) return VplView();
    std::vector<zigzag::CellRef> dropped;
    if (n >= 0) {
      std::size_t skip = std::min(static_cast<std::size_t>(n), cells.size());
      dropped.assign(cells.begin() + skip, cells.end());
    } else {
      std::size_t skip = std::min(static_cast<std::size_t>(-n), cells.size());
      dropped.assign(cells.begin(), cells.end() - skip);
    }
    zigzag::DimRef axis =
        right.axes().empty() ? defaultDim() : right.axes()[0].dim;
    for (std::size_t i = 1; i < dropped.size(); ++i) {
      arena().link(dropped[i - 1], axis, zigzag::DimVector::POS, dropped[i]);
    }
    VplView res(dropped.empty() ? zigzag::noCell : dropped[0],
                {zigzag::DirectedDim{axis, zigzag::DimVector::POS}});
    res.setExtents({dropped.size()});
    return res;
  }

  // ∊ (Member)
  if (verb == TokenKind::Member) {
    auto rightCells = right.collectCells(arena());
    double targetVal =
        left.isScalar()
            ? (left.isFloat() ? left.scalarFloat()
                              : static_cast<double>(left.scalarInt()))
            : 0.0;
    bool found = false;
    for (zigzag::CellRef rc : rightCells) {
      if (cellValueDouble(rc) == targetVal) {
        found = true;
        break;
      }
    }
    return VplView::makeScalar(static_cast<std::int64_t>(found ? 1 : 0));
  }

  // ≡ (Match: address identity or exact value match)
  if (verb == TokenKind::Match) {
    if (left.origin() != zigzag::noCell && right.origin() != zigzag::noCell) {
      return VplView::makeScalar(
          static_cast<std::int64_t>(left.origin() == right.origin() ? 1 : 0));
    }
    bool match = (left.scalarInt() == right.scalarInt() &&
                  left.scalarFloat() == right.scalarFloat());
    return VplView::makeScalar(static_cast<std::int64_t>(match ? 1 : 0));
  }

  // ⍫ (Scrub)
  if (verb == TokenKind::Scrub) {
    std::int64_t t = right.isScalar() ? right.scalarInt() : left.scalarInt();
    VplView targetView = right.isScalar() ? left : right;
    if (store_) {
      (void)store_->rebuildManifoldFromIndex(static_cast<std::uint32_t>(t));
    }
    return targetView;
  }

  // Dyadic math and logic (+, -, ×, ÷, |, *, ⌊, ⌈, =, ≠, <, ≤, >, ≥, ∧, ∨)
  if (left.isScalar() && right.isScalar()) {
    double l   = left.isFloat() ? left.scalarFloat()
                                : static_cast<double>(left.scalarInt());
    double r   = right.isFloat() ? right.scalarFloat()
                                 : static_cast<double>(right.scalarInt());
    double res = computeDyadicScalar(verb, l, r);
    bool isFl  = left.isFloat() || right.isFloat();
    return VplView::makeScalar(res, isFl);
  }

  if (left.isScalar()) {
    bool isInt = !left.isFloat() && !right.isFloat();
    if (isInt) {
      std::int64_t l = left.scalarInt();
      auto rCells    = right.collectCells(arena());
      std::vector<std::int64_t> vals;
      for (zigzag::CellRef rc : rCells) {
        vals.push_back(static_cast<std::int64_t>(computeDyadicScalar(
            verb, static_cast<double>(l), cellValueDouble(rc))));
      }
      return mintRank(vals, right.axes().empty() ? defaultDim()
                                                 : right.axes()[0].dim);
    }
    double l    = left.isFloat() ? left.scalarFloat()
                                 : static_cast<double>(left.scalarInt());
    auto rCells = right.collectCells(arena());
    std::vector<double> vals;
    for (zigzag::CellRef rc : rCells) {
      vals.push_back(computeDyadicScalar(verb, l, cellValueDouble(rc)));
    }
    return mintRank(vals,
                    right.axes().empty() ? defaultDim() : right.axes()[0].dim);
  }

  if (right.isScalar()) {
    bool isInt = !left.isFloat() && !right.isFloat();
    if (isInt) {
      std::int64_t r = right.scalarInt();
      auto lCells    = left.collectCells(arena());
      std::vector<std::int64_t> vals;
      for (zigzag::CellRef lc : lCells) {
        vals.push_back(static_cast<std::int64_t>(computeDyadicScalar(
            verb, cellValueDouble(lc), static_cast<double>(r))));
      }
      return mintRank(vals,
                      left.axes().empty() ? defaultDim() : left.axes()[0].dim);
    }
    double r    = right.isFloat() ? right.scalarFloat()
                                  : static_cast<double>(right.scalarInt());
    auto lCells = left.collectCells(arena());
    std::vector<double> vals;
    for (zigzag::CellRef lc : lCells) {
      vals.push_back(computeDyadicScalar(verb, cellValueDouble(lc), r));
    }
    return mintRank(vals,
                    left.axes().empty() ? defaultDim() : left.axes()[0].dim);
  }

  bool isInt        = !left.isFloat() && !right.isFloat();
  auto lCells       = left.collectCells(arena());
  auto rCells       = right.collectCells(arena());
  std::size_t count = std::min(lCells.size(), rCells.size());
  if (isInt) {
    std::vector<std::int64_t> vals;
    for (std::size_t i = 0; i < count; ++i) {
      vals.push_back(static_cast<std::int64_t>(computeDyadicScalar(
          verb, cellValueDouble(lCells[i]), cellValueDouble(rCells[i]))));
    }
    return mintRank(vals,
                    left.axes().empty() ? defaultDim() : left.axes()[0].dim);
  }
  std::vector<double> vals;
  for (std::size_t i = 0; i < count; ++i) {
    vals.push_back(computeDyadicScalar(verb, cellValueDouble(lCells[i]),
                                       cellValueDouble(rCells[i])));
  }
  return mintRank(vals,
                  left.axes().empty() ? defaultDim() : left.axes()[0].dim);
}

double VPLEngine::computeMonadicScalar(TokenKind verb, double val) {
  switch (verb) {
  case TokenKind::Plus:
    return val;
  case TokenKind::Minus:
    return -val;
  case TokenKind::Times:
    if (val > 0) return 1.0;
    if (val < 0) return -1.0;
    return 0.0;
  case TokenKind::Divide:
    return val != 0.0 ? 1.0 / val : 0.0;
  case TokenKind::Magnitude:
    return std::abs(val);
  case TokenKind::Not:
    return (val == 0.0) ? 1.0 : 0.0;
  case TokenKind::Floor:
    return std::floor(val);
  case TokenKind::Ceiling:
    return std::ceil(val);
  default:
    return val;
  }
}

double VPLEngine::computeDyadicScalar(TokenKind verb, double left,
                                      double right) {
  switch (verb) {
  case TokenKind::Plus:
    return left + right;
  case TokenKind::Minus:
    return left - right;
  case TokenKind::Times:
    return left * right;
  case TokenKind::Divide:
    return right != 0.0 ? left / right : 0.0;
  case TokenKind::Magnitude:
    return std::fmod(left, right != 0.0 ? right : 1.0);
  case TokenKind::Power:
    return std::pow(left, right);
  case TokenKind::Floor:
    return std::min(left, right);
  case TokenKind::Ceiling:
    return std::max(left, right);
  case TokenKind::Equal:
    return (left == right) ? 1.0 : 0.0;
  case TokenKind::NotEqual:
    return (left != right) ? 1.0 : 0.0;
  case TokenKind::LessThan:
    return (left < right) ? 1.0 : 0.0;
  case TokenKind::LessEqual:
    return (left <= right) ? 1.0 : 0.0;
  case TokenKind::GreaterThan:
    return (left > right) ? 1.0 : 0.0;
  case TokenKind::GreaterEqual:
    return (left >= right) ? 1.0 : 0.0;
  case TokenKind::And:
    return (left != 0.0 && right != 0.0) ? 1.0 : 0.0;
  case TokenKind::Or:
    return (left != 0.0 || right != 0.0) ? 1.0 : 0.0;
  default:
    return right;
  }
}

double VPLEngine::cellValueDouble(zigzag::CellRef cell) const {
  if (cell == zigzag::noCell) return 0.0;
  if (auto d = arena().asDouble(cell); d.has_value()) return *d;
  if (auto n = arena().asInt64(cell); n.has_value())
    return static_cast<double>(*n);
  if (auto b = arena().asBool(cell); b.has_value()) return *b ? 1.0 : 0.0;
  std::string s = arena().textOf(cell);
  if (!s.empty()) {
    try {
      return std::stod(s);
    } catch (...) {
    }
  }
  return 0.0;
}

std::int64_t VPLEngine::cellValueInt(zigzag::CellRef cell) const {
  if (cell == zigzag::noCell) return 0;
  if (auto n = arena().asInt64(cell); n.has_value()) return *n;
  if (auto d = arena().asDouble(cell); d.has_value())
    return static_cast<std::int64_t>(*d);
  if (auto b = arena().asBool(cell); b.has_value()) return *b ? 1 : 0;
  std::string s = arena().textOf(cell);
  if (!s.empty()) {
    try {
      return std::stoll(s);
    } catch (...) {
    }
  }
  return 0;
}

std::string VPLEngine::cellValueString(zigzag::CellRef cell) const {
  if (cell == zigzag::noCell) return "";
  return arena().textOf(cell);
}

VplView VPLEngine::mintRank(const std::vector<double> &values,
                            zigzag::DimRef dim) {
  if (values.empty()) return VplView();
  zigzag::CellRef head = arena().makeScalarCell(values[0]);
  zigzag::CellRef prev = head;
  for (std::size_t i = 1; i < values.size(); ++i) {
    zigzag::CellRef c = arena().makeScalarCell(values[i]);
    arena().link(prev, dim, zigzag::DimVector::POS, c);
    prev = c;
  }
  VplView res(head, {zigzag::DirectedDim{dim, zigzag::DimVector::POS}});
  res.setExtents({values.size()});
  res.setScalarPayload(values[0]);
  return res;
}

VplView VPLEngine::mintRank(const std::vector<std::int64_t> &values,
                            zigzag::DimRef dim) {
  if (values.empty()) return VplView();
  zigzag::CellRef head = arena().makeScalarCell(values[0]);
  zigzag::CellRef prev = head;
  for (std::size_t i = 1; i < values.size(); ++i) {
    zigzag::CellRef c = arena().makeScalarCell(values[i]);
    arena().link(prev, dim, zigzag::DimVector::POS, c);
    prev = c;
  }
  VplView res(head, {zigzag::DirectedDim{dim, zigzag::DimVector::POS}});
  res.setExtents({values.size()});
  res.setScalarPayload(values[0]);
  return res;
}

VplView VPLEngine::mintRank(const std::vector<std::string> &values,
                            zigzag::DimRef dim) {
  if (values.empty()) return VplView();
  zigzag::CellRef head = arena().makeCell(values[0]);
  zigzag::CellRef prev = head;
  for (std::size_t i = 1; i < values.size(); ++i) {
    zigzag::CellRef c = arena().makeCell(values[i]);
    arena().link(prev, dim, zigzag::DimVector::POS, c);
    prev = c;
  }
  VplView res(head, {zigzag::DirectedDim{dim, zigzag::DimVector::POS}});
  res.setExtents({values.size()});
  return res;
}

} // namespace xanadu::vpl
