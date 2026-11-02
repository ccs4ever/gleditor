// Layout-only prototypes; no VQL lowering or runtime performance is implied.
#include <cstdint>

struct VqlHyperHandlePrototype {
  std::uint64_t identity, context;
};
struct VqlCurriedMorphismPrototype {
  VqlHyperHandlePrototype subject;
  std::uint64_t operation;
};
static_assert(sizeof(VqlHyperHandlePrototype) == 16);
static_assert(sizeof(VqlCurriedMorphismPrototype) == 24);

extern "C" [[gnu::noinline]] std::uint64_t
vql_probe_handle(VqlHyperHandlePrototype handle) {
  return handle.identity + handle.context;
}
extern "C" [[gnu::noinline]] std::uint64_t
vql_probe_morphism(VqlCurriedMorphismPrototype morphism) {
  return morphism.subject.identity + morphism.subject.context +
         morphism.operation;
}
