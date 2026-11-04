#ifndef COMMON_XANADU_VAGENT_P16_HPP
#define COMMON_XANADU_VAGENT_P16_HPP

// vagent-side spelling for the P16 ceremony seam. The abstract interface
// (`P16Flag`, `P16Ceremony`) is defined in `vortex/p16_ceremony.hpp` —
// the layer that drives it — and implemented here by
// `p16.cpp`'s EngineP16Ceremony (installed per actor via
// `MeshActorRuntime::setP16Ceremony`). These aliases keep the
// `xanadu::vagent::` spelling for the implementation side while the
// canonical type lives at the lower layer; the dependency points down.

#include "common/xanadu/vortex/p16_ceremony.hpp"

namespace xanadu::vagent {

using zigzag::vortex::P16Ceremony;
using zigzag::vortex::P16Flag;

} // namespace xanadu::vagent

#endif // COMMON_XANADU_VAGENT_P16_HPP
