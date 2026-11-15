/**
 * @file execution.cpp
 * @brief Frontend-neutral VQL execution and error normalization.
 */
#include "common/xanadu/vql/execution.hpp"

#include <algorithm>
#include <cctype>
#include <iterator>
#include <optional>
#include <ranges>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "common/xanadu/multi_store.hpp"
#include "common/xanadu/vql/compiler.hpp"
#include "common/xanadu/vql/contracts.hpp"
#include "common/xanadu/vql/frame_eligibility.hpp"
#include "common/xanadu/vql/parser.hpp"
#include "common/xanadu/vql/prose_hits.hpp"
#include "common/xanadu/vql/vql_engine.hpp"

namespace xanadu::vql {
namespace {

template <typename Action> struct RestoreScope {
  Action action;
  ~RestoreScope() { action(); }
};

void validateSignatures(const QueryExpression &query) {
  for (const auto &definition : query.defs)
    validateFunctionSignature(definition);
}

bool budgetFailure(const std::string_view message) {
  std::string lowered;
  lowered.reserve(message.size());
  std::ranges::transform(message, std::back_inserter(lowered), [](char c) {
    return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  });
  return lowered.find("budget") != std::string::npos ||
         lowered.find("max cycles") != std::string::npos ||
         lowered.find("work limit") != std::string::npos ||
         lowered.find("activation frame depth limit") != std::string::npos ||
         (lowered.find("scoped choice") != std::string::npos &&
          lowered.find("limit") != std::string::npos) ||
         (lowered.find("limit") != std::string::npos &&
          (lowered.find("guard-scope") != std::string::npos ||
           lowered.find("mutation-group") != std::string::npos ||
           lowered.find("projection") != std::string::npos ||
           lowered.find("vqlmasklinks") != std::string::npos));
}

QueryExecutionError runtimeError(const std::exception &error) {
  if (const auto *refusal = dynamic_cast<const QueryRefusal *>(&error))
    return {.category = refusal->category(), .message = error.what()};
  return {.category = budgetFailure(error.what())
                          ? QueryExecutionError::Category::Budget
                          : QueryExecutionError::Category::Runtime,
          .message  = error.what()};
}

bool declaresOnly(const QueryExpression &query) {
  if (!query.defs.empty() || !query.body.statements.empty()) return false;
  if (query.mounts.empty() && query.namespaceDeclarations.empty()) return false;
  const auto *block = std::get_if<ExecutionBlock>(&query.expr);
  if (block == nullptr || !block->bindings.empty() || block->where)
    return false;
  const auto *ret = std::get_if<ReturnClause>(&block->action.clause);
  return ret != nullptr && ret->items.empty();
}

void publishMounts(
    VQLEngine *session,
    const std::vector<CompilationResult::SessionBinding> &events) {
  if (session == nullptr) return;
  using Kind = CompilationResult::SessionBinding::Kind;
  for (const auto &event : events) {
    if (event.kind == Kind::Mount)
      session->publishMount(
          event.name, VQLEngine::SessionMount{.direction = event.direction,
                                              .roots     = event.slots});
    else if (event.kind == Kind::Unmount)
      session->publishUnmount(event.name);
  }
}

} // namespace

QueryExecutor::QueryExecutor(MultiStoreCoordinator &coordinator,
                             zigzag::vortex::VortexVM &vm,
                             QueryExecutionOptions options,
                             ContractBindings bindings)
    : coordinator_(coordinator), bindings_(std::move(bindings)), vm_(vm),
      options_(options) {}

QueryExecutor::QueryExecutor(VQLEngine &session, zigzag::vortex::VortexVM &vm,
                             QueryExecutionOptions options)
    : QueryExecutor(session.coordinator(), vm, options) {
  session_       = &session;
  bindingSource_ = std::cref(session.variables());
}

std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
QueryExecutor::execute(const std::string_view source,
                       const std::vector<zigzag::CellRef> &contextCells) {
  try {
    const auto query = Parser(source).parseQuery();
    return execute(query, contextCells);
  } catch (const ParseError &error) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Parse,
        .message  = error.what(),
    });
  } catch (const std::exception &error) {
    return std::unexpected(runtimeError(error));
  }
}

std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
QueryExecutor::execute(const QueryExpression &query,
                       const std::vector<zigzag::CellRef> &contextCells) {
  disassembly_.clear();
  if (&coordinator_.core() != &vm_.core()) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Runtime,
        .message  = "VQL executor engines must share one VortexCore",
    });
  }
  QueryExpression effectiveQuery = query;
  if (session_ != nullptr) {
    for (const auto &[name, def] : session_->userDefs()) {
      if (std::ranges::none_of(
              effectiveQuery.defs,
              [&](const DefDeclaration &d) { return d.name == name; })) {
        effectiveQuery.defs.push_back(def);
      }
    }
  }
  try {
    const auto eligibility = analyzeFrameEligibility(effectiveQuery);
    if (!eligibility)
      return std::unexpected(QueryExecutionError{
          .category = QueryExecutionError::Category::Compile,
          .message  = eligibility.error().message});
    validateSignatures(effectiveQuery);
  } catch (const TypeContractError &error) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::TypeContract,
        .message  = error.what(),
    });
  } catch (const std::exception &error) {
    return std::unexpected(runtimeError(error));
  }
  if (options_.mode != QueryExecutionMode::Vortex)
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Compile,
        .message =
            "direct execution was removed; VQL requires Vortex compilation",
    });
  return executeVortex(effectiveQuery, contextCells);
}

std::expected<std::vector<zigzag::CellRef>, QueryExecutionError>
QueryExecutor::executeVortex(const QueryExpression &query,
                             const std::vector<zigzag::CellRef> &contextCells) {
  if (options_.maxCycles == 0) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::Budget,
        .message  = "VQL execution cycle budget must be positive",
    });
  }
  const auto priorContractCoordinator = vm_.contractCoordinator();
  const RestoreScope restoreContractCoordinator{
      [this, priorContractCoordinator] {
        vm_.setContractCoordinator(priorContractCoordinator);
      }};
  try {
    const auto programMark = coordinator_.arena().mark();
    bool programCommitted  = false;
    const RestoreScope unwindMark{[this, programMark, &programCommitted] {
      if (!programCommitted) {
        std::ignore = coordinator_.arena().release(programMark);
        vm_.pruneAssemblyState();
      }
    }};

    // PROSE-HITS REGISTRATION SITE
    struct ProseHitsCache {
      std::string lastNeedle;
      bool hasNeedle{false};
      std::vector<zigzag::CellRef> hits;
    };
    auto cache = std::make_shared<ProseHitsCache>();
    const RestoreScope restoreProseHits{
        [this] { vm_.setRelationProvider("prose_hits", nullptr); }};
    vm_.setRelationProvider(
        "prose_hits",
        [this, cache](const zigzag::vortex::RelationQuery &rq)
            -> zigzag::vortex::RelationResult {
          if (rq.arguments.empty()) {
            return std::unexpected("prose_hits expects 1 needle argument");
          }
          const auto needleCell    = rq.arguments[0];
          const std::string needle = rq.arena.textOf(needleCell);
          if (!cache->hasNeedle || cache->lastNeedle != needle) {
            cache->lastNeedle = needle;
            cache->hasNeedle  = true;
            cache->hits       = proseHits(coordinator_, needle, rq.arena);
          }
          if (rq.position < 0 ||
              static_cast<std::size_t>(rq.position) >= cache->hits.size()) {
            return std::optional<zigzag::vortex::RelationAnswer>{std::nullopt};
          }
          const auto hitCell =
              cache->hits[static_cast<std::size_t>(rq.position)];
          return std::optional<zigzag::vortex::RelationAnswer>{
              zigzag::vortex::RelationAnswer{
                  .nextPosition = rq.position + 1,
                  .limit        = static_cast<std::int64_t>(cache->hits.size()),
                  .answers      = {hitCell},
              }};
        });

    VQLCompiler compiler(coordinator_, vm_);
    CompilationOptions compilation;
    compilation.contextCells = contextCells;
    compilation.initialBindings =
        bindingSource_ ? bindingSource_->get() : bindings_;
    if (session_) {
      compilation.dynamicViews    = &session_->dynamicViews();
      compilation.initialFunctors = session_->compilerFunctors();
      for (const auto &[name, mount] : session_->compilerMounts())
        compilation.mounts.emplace(
            name, CompilationOptions::Mount{.direction = mount.direction,
                                            .roots     = mount.roots});
    }
    if (declaresOnly(query)) {
      programCommitted  = true;
      const auto events = compiler.declareMounts(query, compilation);
      publishMounts(session_, events);
      return std::vector<zigzag::CellRef>{};
    }
    const auto result = compiler.compile(query, compilation);
    disassembly_      = result.disassembly;
    if (!result.success) {
      return std::unexpected(QueryExecutionError{
          .category = result.category.value_or(QueryErrorCategory::Unsupported),
          .message  = result.errorMessage,
      });
    }

    if (!result.cursor || !result.terminalOpcode) {
      return std::unexpected(QueryExecutionError{
          .category = QueryExecutionError::Category::Compile,
          .message  = "compiled VQL has no executable activation metadata",
      });
    }
    const auto cursor = *result.cursor;

    // A frontend request has no resume handle. Keep raw VM stops resumable,
    // but release this request's cursor on success, failure, or exception.
    const RestoreScope finishRequest{[this, id = cursor] {
      vm_.abortScopedChoices(id);
      vm_.setCursorOpcode(id, std::nullopt);
    }};
    const auto execution = vm_.run(cursor, options_.maxCycles);
    if (!execution.success) {
      const auto category =
          execution.contractViolation ==
                  zigzag::vortex::ContractViolationKind::Postcondition
              ? QueryExecutionError::Category::TypeContract
              : (budgetFailure(execution.errorMessage)
                     ? QueryExecutionError::Category::Budget
                     : QueryExecutionError::Category::Runtime);
      return std::unexpected(QueryExecutionError{
          .category = category,
          .message  = execution.errorMessage,
      });
    }

    std::vector<zigzag::CellRef> cells;
    for (const auto slot : result.resultSlots) {
      const auto values = vm_.resultCells(slot);
      cells.insert(cells.end(), values.begin(), values.end());
    }

    auto &arena = coordinator_.arena();
    std::unordered_map<zigzag::CellRef, zigzag::CellRef> remap;
    std::unordered_map<std::string, std::vector<zigzag::CellRef>>
        sessionBindingCells;
    if (session_) {
      for (const auto &binding : result.sessionBindings) {
        for (const auto slot : binding.slots) {
          for (const auto c : vm_.resultCells(slot)) {
            sessionBindingCells[binding.name].push_back(c);
          }
        }
      }
    }
    const bool hasMutations = arena.trailSize() > programMark.trailSize ||
                              arena.spaceCount() != programMark.spaceCount;

    if (hasMutations) {
      programCommitted = true;
      zigzag::expectMarkOrder(arena.discard(programMark));
    } else {
      bool hasNewResultCells = false;
      for (const auto c : cells) {
        if (const auto dense = arena.denseOf(c);
            dense.has_value() && *dense >= programMark.cellCount) {
          hasNewResultCells = true;
          break;
        }
      }
      if (!hasNewResultCells && session_) {
        for (const auto &binding : result.sessionBindings) {
          for (const auto slot : binding.slots) {
            for (const auto c : vm_.resultCells(slot)) {
              if (const auto dense = arena.denseOf(c);
                  dense.has_value() && *dense >= programMark.cellCount) {
                hasNewResultCells = true;
                break;
              }
            }
            if (hasNewResultCells) break;
          }
          if (hasNewResultCells) break;
        }
        if (!hasNewResultCells) {
          const auto &dv = session_->dynamicViews();
          for (const auto &[k, v] : dv.viewCells) {
            if (const auto dense = arena.denseOf(k);
                dense.has_value() && *dense >= programMark.cellCount) {
              hasNewResultCells = true;
              break;
            }
          }
          if (!hasNewResultCells) {
            for (const auto &[k, v] : dv.shapeHandles) {
              if (const auto dense = arena.denseOf(k);
                  dense.has_value() && *dense >= programMark.cellCount) {
                hasNewResultCells = true;
                break;
              }
            }
          }
          if (!hasNewResultCells) {
            for (const auto &[k, v] : dv.cellLenses) {
              if (const auto dense = arena.denseOf(k);
                  dense.has_value() && *dense >= programMark.cellCount) {
                hasNewResultCells = true;
                break;
              }
            }
          }
        }
      }

      if (!hasNewResultCells) {
        programCommitted = true;
        zigzag::expectMarkOrder(arena.release(programMark));
        vm_.pruneAssemblyState();
      } else {
        // Card O2b: copy-out program-minted result cells and release
        // programMark
        std::unordered_set<zigzag::CellRef> visited;
        std::vector<zigzag::CellRef> queue;

        const auto isVmInternalDim = [&](zigzag::DimRef dim) {
          const auto &sys = coordinator_.core().dims();
          return dim == sys.dims || dim == sys.grab || dim == sys.step ||
                 dim == sys.spin || dim == sys.stack || dim == sys.contract ||
                 dim == sys.cursors || dim == sys.cache || dim == sys.vars ||
                 dim == sys.values || dim == sys.pinningCursors ||
                 dim == sys.stdlib || dim == sys.clause || dim == sys.scan ||
                 (sys.runtimeObjectClaim && dim == *sys.runtimeObjectClaim);
        };

        const auto enqueue = [&](const zigzag::CellRef c) {
          if (c == zigzag::noCell) return;
          const auto dense = arena.denseOf(c);
          if (dense.has_value() && *dense >= programMark.cellCount &&
              visited.insert(c).second) {
            queue.push_back(c);
          }
        };

        for (const auto c : cells) enqueue(c);
        if (session_) {
          for (const auto &binding : result.sessionBindings) {
            for (const auto slot : binding.slots) {
              for (const auto c : vm_.resultCells(slot)) enqueue(c);
            }
          }
          const auto &dv = session_->dynamicViews();
          for (const auto &[k, v] : dv.viewCells) {
            enqueue(k);
            enqueue(v.source);
            if (v.view) {
              for (const auto &[hk, hv] : v.view->handles) {
                enqueue(hk);
                enqueue(hv);
              }
            }
          }
          for (const auto &[k, v] : dv.ownerViews) {
            enqueue(k);
            enqueue(v);
          }
          for (const auto &[k, v] : dv.shapeHandles) {
            enqueue(k);
          }
          for (const auto &[k, v] : dv.cellLenses) {
            enqueue(k);
          }
        }
        for (std::size_t i = 0; i < programMark.cellCount; ++i) {
          const auto c = arena.refOf(static_cast<std::uint32_t>(i));
          for (const auto &entry : arena.dimensionsOf(c)) {
            if (isVmInternalDim(entry.dim)) continue;
            if (entry.pos != zigzag::noCell) enqueue(entry.pos);
            if (entry.neg != zigzag::noCell) enqueue(entry.neg);
          }
        }

        struct SnapshotCell {
          zigzag::CellRef oldRef{zigzag::noCell};
          bool isProxy{false};
          std::uint32_t proxySpace{0};
          zigzag::CellRef proxyIndex{zigzag::noCell};
          xanadu::ValueKind valueKind{xanadu::ValueKind::None};
          std::uint64_t valueBits{0};
          std::vector<xanadu::PrimediaSpan> content;
          std::optional<std::uint32_t> quoteSpace;
          std::string scratchText;
          struct LinkEntry {
            zigzag::DimRef dim{zigzag::noCell};
            zigzag::DimVector dir{zigzag::DimVector::POS};
            zigzag::CellRef target{zigzag::noCell};
          };
          std::vector<LinkEntry> links;
        };

        std::vector<SnapshotCell> snapshotCells;
        for (std::size_t q = 0; q < queue.size(); ++q) {
          const auto c = queue[q];
          SnapshotCell sc;
          sc.oldRef = c;
          if (const auto foreign = arena.foreignOf(c)) {
            sc.isProxy    = true;
            sc.proxySpace = foreign->space;
            sc.proxyIndex = foreign->index;
          }
          if (const auto s = arena.slot(c); s.has_value()) {
            sc.valueKind = static_cast<xanadu::ValueKind>(s->valueKind);
            sc.valueBits = s->valueBits;
          }
          const auto spans = arena.contentOf(c);
          sc.content.assign(spans.begin(), spans.end());
          if (const auto foreign = arena.foreignOf(c)) {
            if (const auto s = arena.spaceAt(foreign->space)) {
              if (s->store) {
                sc.quoteSpace = foreign->space;
                if (s->manifold) {
                  const auto foreignSpans =
                      s->manifold->contentOf(foreign->index);
                  sc.content.assign(foreignSpans.begin(), foreignSpans.end());
                }
              }
            }
          } else if (const auto quote = arena.quotedContent(c)) {
            for (std::uint32_t s = 1; s <= arena.spaceCount(); ++s) {
              if (const auto att = arena.spaceAt(s);
                  att && att->store == quote->store) {
                sc.quoteSpace = s;
                sc.content.assign(quote->spans.begin(), quote->spans.end());
                break;
              }
            }
          }
          for (const auto &span : sc.content) {
            if (span.scroll == xanadu::scratchScroll) {
              sc.scratchText = arena.textOf(c);
              break;
            }
          }
          for (const auto &entry : arena.dimensionsOf(c)) {
            if (isVmInternalDim(entry.dim)) continue;
            if (entry.pos != zigzag::noCell) {
              sc.links.push_back(
                  {entry.dim, zigzag::DimVector::POS, entry.pos});
              enqueue(entry.pos);
            }
            if (entry.neg != zigzag::noCell) {
              sc.links.push_back(
                  {entry.dim, zigzag::DimVector::NEG, entry.neg});
              enqueue(entry.neg);
            }
          }
          snapshotCells.push_back(std::move(sc));
        }

        struct PreExistingModification {
          zigzag::CellRef cell{zigzag::noCell};
          xanadu::ValueKind valueKind{xanadu::ValueKind::None};
          std::uint64_t valueBits{0};
          std::vector<xanadu::PrimediaSpan> content;
          std::string scratchText;
          std::vector<SnapshotCell::LinkEntry> links;
        };
        std::vector<PreExistingModification> modifiedPreExisting;
        if (arena.trailSize() > programMark.trailSize) {
          for (std::size_t i = 0; i < programMark.cellCount; ++i) {
            const auto c = arena.refOf(static_cast<std::uint32_t>(i));
            PreExistingModification mod;
            mod.cell = c;
            if (const auto s = arena.slot(c); s.has_value()) {
              mod.valueKind = static_cast<xanadu::ValueKind>(s->valueKind);
              mod.valueBits = s->valueBits;
            }
            const auto spans = arena.contentOf(c);
            mod.content.assign(spans.begin(), spans.end());
            for (const auto &span : mod.content) {
              if (span.scroll == xanadu::scratchScroll) {
                mod.scratchText = arena.textOf(c);
                break;
              }
            }
            for (const auto &entry : arena.dimensionsOf(c)) {
              if (isVmInternalDim(entry.dim)) continue;
              if (entry.pos != zigzag::noCell) {
                mod.links.push_back(
                    {entry.dim, zigzag::DimVector::POS, entry.pos});
              }
              if (entry.neg != zigzag::noCell) {
                mod.links.push_back(
                    {entry.dim, zigzag::DimVector::NEG, entry.neg});
              }
            }
            modifiedPreExisting.push_back(std::move(mod));
          }
        }

        programCommitted        = true;
        auto releaseMark        = programMark;
        const auto currentMark  = arena.mark();
        std::ignore             = arena.discard(currentMark);
        releaseMark.scratchSize = currentMark.scratchSize;
        releaseMark.contentSize = currentMark.contentSize;
        releaseMark.spaceCount  = currentMark.spaceCount;
        zigzag::expectMarkOrder(arena.release(releaseMark));
        vm_.pruneAssemblyState();

        for (const auto &sc : snapshotCells) {
          zigzag::CellRef newCell = zigzag::noCell;
          if (sc.isProxy) {
            newCell = arena.proxyFor(sc.proxySpace, sc.proxyIndex);
          } else if (sc.quoteSpace) {
            newCell = arena.makeQuote(*sc.quoteSpace, sc.content);
            if (sc.valueKind != xanadu::ValueKind::None) {
              zigzag::expectWritten(
                  arena.setValueBits(newCell, sc.valueKind, sc.valueBits));
            }
          } else if (sc.valueKind == xanadu::ValueKind::Int64) {
            newCell =
                arena.makeScalarCell(std::bit_cast<std::int64_t>(sc.valueBits));
          } else if (sc.valueKind == xanadu::ValueKind::Double) {
            newCell = arena.makeScalarCell(std::bit_cast<double>(sc.valueBits));
          } else if (sc.valueKind == xanadu::ValueKind::Bool) {
            newCell = arena.makeScalarCell(sc.valueBits != 0);
          } else if (sc.valueKind != xanadu::ValueKind::None) {
            newCell = arena.makeCell();
            zigzag::expectWritten(
                arena.setValueBits(newCell, sc.valueKind, sc.valueBits));
          } else {
            newCell = arena.makeCell();
            if (!sc.content.empty()) {
              zigzag::expectWritten(arena.setContent(newCell, sc.content));
            } else if (!sc.scratchText.empty()) {
              const auto span = arena.intern(sc.scratchText);
              zigzag::expectWritten(
                  arena.setContent(newCell, std::span{&span, 1}));
            }
          }
          remap[sc.oldRef] = newCell;
        }

        for (const auto &sc : snapshotCells) {
          const auto from = remap.at(sc.oldRef);
          for (const auto &link : sc.links) {
            if (isVmInternalDim(link.dim)) continue;
            const auto to = remap.contains(link.target) ? remap.at(link.target)
                                                        : link.target;
            if (!arena.contains(to)) continue;
            if (arena.linked(from, link.dim, link.dir) != to) {
              zigzag::expectWritten(arena.link(from, link.dim, link.dir, to));
            }
          }
        }

        for (const auto &mod : modifiedPreExisting) {
          if (const auto s = arena.slot(mod.cell);
              s.has_value() &&
              (mod.valueKind != static_cast<xanadu::ValueKind>(s->valueKind) ||
               mod.valueBits != s->valueBits)) {
            zigzag::expectWritten(
                arena.setValueBits(mod.cell, mod.valueKind, mod.valueBits));
          }
          if (const auto curContent = arena.contentOf(mod.cell);
              !std::ranges::equal(curContent, mod.content)) {
            if (!mod.content.empty()) {
              zigzag::expectWritten(arena.setContent(mod.cell, mod.content));
            } else if (!mod.scratchText.empty()) {
              const auto span = arena.intern(mod.scratchText);
              zigzag::expectWritten(
                  arena.setContent(mod.cell, std::span{&span, 1}));
            }
          }
          for (const auto &entry : arena.dimensionsOf(mod.cell)) {
            if (isVmInternalDim(entry.dim)) continue;
            for (const auto dir :
                 {zigzag::DimVector::POS, zigzag::DimVector::NEG}) {
              const auto curTgt = arena.linked(mod.cell, entry.dim, dir);
              if (!curTgt) continue;
              const bool shouldHave =
                  std::ranges::any_of(mod.links, [&](const auto &l) {
                    const auto desiredTgt = remap.contains(l.target)
                                                ? remap.at(l.target)
                                                : l.target;
                    return l.dim == entry.dim && l.dir == dir &&
                           desiredTgt == *curTgt;
                  });
              if (!shouldHave) {
                zigzag::expectWritten(arena.unlink(mod.cell, entry.dim, dir));
              }
            }
          }
          for (const auto &link : mod.links) {
            if (isVmInternalDim(link.dim)) continue;
            const auto desiredTgt = remap.contains(link.target)
                                        ? remap.at(link.target)
                                        : link.target;
            if (!arena.contains(desiredTgt)) continue;
            if (arena.linked(mod.cell, link.dim, link.dir) != desiredTgt) {
              zigzag::expectWritten(
                  arena.link(mod.cell, link.dim, link.dir, desiredTgt));
            }
          }
        }
      }
    }

    for (auto &c : cells) {
      if (remap.contains(c)) c = remap.at(c);
    }

    if (session_ && !remap.empty()) {
      auto &dv = session_->dynamicViews();
      std::unordered_map<zigzag::CellRef, ViewCellRecord> nextViewCells;
      for (auto &[k, v] : dv.viewCells) {
        const auto newK = remap.contains(k) ? remap.at(k) : k;
        auto newV       = v;
        if (remap.contains(newV.source)) newV.source = remap.at(newV.source);
        if (newV.view) {
          std::unordered_map<zigzag::CellRef, zigzag::CellRef> newHandles;
          for (auto &[hk, hv] : newV.view->handles) {
            const auto nhk  = remap.contains(hk) ? remap.at(hk) : hk;
            const auto nhv  = remap.contains(hv) ? remap.at(hv) : hv;
            newHandles[nhk] = nhv;
          }
          newV.view->handles = std::move(newHandles);
        }
        nextViewCells[newK] = std::move(newV);
      }
      dv.viewCells = std::move(nextViewCells);

      std::unordered_map<zigzag::CellRef, zigzag::CellRef> nextOwnerViews;
      for (auto &[k, v] : dv.ownerViews) {
        const auto newK      = remap.contains(k) ? remap.at(k) : k;
        const auto newV      = remap.contains(v) ? remap.at(v) : v;
        nextOwnerViews[newK] = newV;
      }
      dv.ownerViews = std::move(nextOwnerViews);

      std::unordered_map<zigzag::CellRef, PutGetLensRecord> nextCellLenses;
      for (auto &[k, v] : dv.cellLenses) {
        const auto newK      = remap.contains(k) ? remap.at(k) : k;
        nextCellLenses[newK] = std::move(v);
      }
      dv.cellLenses = std::move(nextCellLenses);

      std::unordered_map<zigzag::CellRef, ShapeLiteral> nextShapeHandles;
      for (auto &[k, v] : dv.shapeHandles) {
        const auto newK        = remap.contains(k) ? remap.at(k) : k;
        nextShapeHandles[newK] = std::move(v);
      }
      dv.shapeHandles = std::move(nextShapeHandles);
    }

    if (session_) {
      ContractBindings published;
      std::unordered_map<std::string, std::vector<PathStep>> functors;
      publishMounts(session_, result.sessionBindings);
      for (const auto &binding : result.sessionBindings) {
        using Kind = CompilationResult::SessionBinding::Kind;
        if (binding.kind == Kind::Mount || binding.kind == Kind::Unmount)
          continue;
        if (binding.kind == CompilationResult::SessionBinding::Kind::Functor) {
          functors.emplace(binding.name, binding.functor);
          continue;
        }
        auto &values = published[binding.name];
        if (const auto found = sessionBindingCells.find(binding.name);
            found != sessionBindingCells.end()) {
          for (auto val : found->second) {
            if (remap.contains(val)) val = remap.at(val);
            values.push_back(val);
          }
        }
      }
      session_->publishCompilerBindings(std::move(published),
                                        std::move(functors));
      session_->publishUserDefs(query.defs);
    }
    programCommitted = true;
    vm_.clearChoicePoints();
    return cells;
  } catch (const TypeContractError &error) {
    return std::unexpected(QueryExecutionError{
        .category = QueryExecutionError::Category::TypeContract,
        .message  = error.what(),
    });
  } catch (const std::exception &error) {
    return std::unexpected(runtimeError(error));
  }
}

} // namespace xanadu::vql
