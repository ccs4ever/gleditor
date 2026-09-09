# Vortex Hyperstructural Runtime & zzstructure System Specification

**Document Version:** 4.0.0 — Definitive Canonical Edition **Target Environment:** Zero-Allocation
Multidimensional Graph Manifolds & Logic Engine

Vortex is a speculative language and runtime design: a programming model whose entire addressable
state is a Zigzag `zzstructure` manifold (the same `d.1`/`d.2`/`d.clone`-style cell-and-dimension
model documented in
[zigzag-multidimensional-space-and-projection.md](zigzag-multidimensional-space-and-projection.md)
and implemented by `apps/zigzag`'s `CompactZZCell`), rather than heaps, stack frames, and registers.
Nothing in this document is wired into the gleditor build; it specifies the language and reference
engine that a future `apps/vortex` (or an embedding inside `apps/zigzag`) would implement. It is
recorded here because it is a design consumer of the same manifold invariants `apps/zigzag` and
`apps/xudu` already enforce, and any future implementation should stay consistent with them.

______________________________________________________________________

## 1. Architectural Foundation & Design Invariants

The Vortex architecture eliminates traditional runtime constructs — linear memory heaps,
hardware-bound register sets, stack frames, and isolated variable lookup tables — in favor of a
unified spatial manifold based on **zzstructures**. Every entity, execution thread, variable
binding, lexical scope, instruction stream, and data structure exists as an interconnected node
within a single multidimensional coordinate matrix.

### The Two-Primitive Invariant

The entire engine core is strictly constrained to two structural primitives and two scalar payload
accessors:

1. **`get_link(cell, dim, direction)`**: Inspects and traverses adjacent cell coordinates along a
   specific dimension.
1. **`set_link(cell, dim, direction, target)`**: Directly establishes, mutates, or breaks
   dimensional connections:
   - **Allocation (`target == -1`)**: Instantiates a fresh cell and wires it directly to the source
     node along the given dimension and direction.
   - **Isolation (`target == 0`)**: Clears the designated directional pointer. When all dimensional
     links of a cell are set to 0, the cell is geometrically isolated.
   - **Identity Fusion & Unfusion (`dim == d_fuse`)**: Establishes or breaks an identity binding
     where multiple cells share a single underlying payload pointer pool.
1. **`get_cell_value(cell, [offset], [length])`**: Dereferences the cell's payload
   (`std::variant<std::string, double, bool>`) with optional virtual slicing.
1. **`set_cell_value(cell, value, [offset], [length])`**: Writes or in-place patches the variant
   payload, updating shared instances across `d.fuse` instantly.

```
                [-d.grab: Output Wing]
                        |
[-d.spin: Past] <-- [Cell Node] --> [+d.spin: Future]
                        |
                [+d.grab: Input Wing]
```

### Topological Garbage Collection

Manual memory deallocation primitives (e.g., `free_cell`) are forbidden. Memory reclamation is
purely reachability-based:

- **Root Set**: Origin Cell (0), the active cursor scheduler rank (`d.cursors`), and global system
  dimension anchors.

- **Trace Cycle**: A mark-and-sweep or reference manifold crawl marks all cells reachable across any
  link.

- **Eager Eviction**: When `set_link` reduces a cell's total non-zero connections across all axes to
  zero:

  ```math
  \sum_{\text{dim}} \left( [\text{links}[\text{dim}].\text{pos} \ne 0] + [\text{links}[\text{dim}].\text{neg} \ne 0] \right) = 0
  ```

  the runtime immediately reclaims the cell without waiting for a full sweep cycle.

______________________________________________________________________

## 2. Core C++ Runtime Engine Reference Implementation

```cpp
#include <iostream>
#include <string>
#include <unordered_map>
#include <memory>
#include <variant>
#include <vector>
#include <algorithm>
#include <cstdint>
#include <cctype>

using cell_id = int64_t;

// Standard System Dimension Coordinates
constexpr cell_id d_grab    = 1;    // Parameter wings (-d.grab = outputs, +d.grab = inputs)
constexpr cell_id d_step    = 2;    // Parameter chaining / sequential rank stepping
constexpr cell_id d_spin    = 3;    // Process instruction stream
constexpr cell_id d_stack   = 4;    // Call frames, choice points, and backtracking trail
constexpr cell_id d_fuse    = 999;  // Quantum identity synchronization
constexpr cell_id d_cursors = 1001; // Process scheduler manifold
constexpr cell_id d_vars    = 1003; // Scope variable names
constexpr cell_id d_values  = 1004; // Variable values / ground terms

using CellValue = std::variant<std::string, double, bool>;

struct Cell {
    cell_id id;
    CellValue primitive_value = "";
    std::unordered_map<cell_id, std::pair<cell_id, cell_id>> links; // [dim] -> {pos, neg}
    std::shared_ptr<CellValue> fused_payload = nullptr;
};

// Global Matrix Storage
static std::unordered_map<cell_id, std::unique_ptr<Cell>> matrix;
static cell_id next_cell_id = 1;

// Internal Allocation Primitive
static cell_id internal_alloc_cell() {
    cell_id id = next_cell_id++;
    auto c = std::make_unique<Cell>();
    c->id = id;
    matrix[id] = std::move(c);
    return id;
}

// Unfuse & Payload Recovery Helper
static void handle_unfuse_cleanup(cell_id c_id) {
    auto& c = matrix[c_id];
    if (!c || !c->fused_payload) return;
    c->primitive_value = *(c->fused_payload);
    if (c->links[d_fuse].first == 0 && c->links[d_fuse].second == 0) {
        c->fused_payload = nullptr;
    } else {
        c->fused_payload =
            std::make_shared<CellValue>(c->primitive_value);
    }
}

// Primitive 1: get_link
cell_id get_link(cell_id cell, cell_id dim, int direction) {
    auto it = matrix.find(cell);
    if (it == matrix.end() || !it->second) return 0;
    auto link_it = it->second->links.find(dim);
    if (link_it == it->second->links.end()) return 0;
    return (direction > 0) ? link_it->second.first :
link_it->second.second;
}

// Primitive 2: set_link
cell_id set_link(cell_id cell, cell_id dim, int direction, cell_id
target) {
    auto& c = matrix[cell];
    if (!c) return 0;

    cell_id actual_target = (target == -1) ? internal_alloc_cell() :
target;

    // Quantum Identity Synchronization along d.fuse
    if (dim == d_fuse) {
        cell_id old_target = (direction > 0) ? c->links[d_fuse].first
: c->links[d_fuse].second;

        // Break Fusion
        if (actual_target == 0 && old_target != 0) {
            if (direction > 0) c->links[d_fuse].first = 0;
            else c->links[d_fuse].second = 0;

            auto& partner = matrix[old_target];
            if (partner) {
                if (direction > 0 && partner->links[d_fuse].second ==
cell) partner->links[d_fuse].second = 0;
                else if (direction < 0 && partner->links[d_fuse].first
== cell) partner->links[d_fuse].first = 0;
                handle_unfuse_cleanup(old_target);
            }
            handle_unfuse_cleanup(cell);
            return 0;
        }

        // Establish Fusion
        if (actual_target != 0) {
            auto& t = matrix[actual_target];
            if (!t) return 0;
            if (direction > 0) { c->links[d_fuse].first =
actual_target; t->links[d_fuse].second = cell; }
            else { c->links[d_fuse].second = actual_target;
t->links[d_fuse].first = cell; }

            if (!c->fused_payload && !t->fused_payload) {
                c->fused_payload =
std::make_shared<CellValue>(c->primitive_value);
                t->fused_payload = c->fused_payload;
            } else if (c->fused_payload && !t->fused_payload) {
                t->fused_payload = c->fused_payload;
            } else if (!c->fused_payload && t->fused_payload) {
                c->fused_payload = t->fused_payload;
            } else if (c->fused_payload != t->fused_payload) {
                *(t->fused_payload) = *(c->fused_payload);
                t->fused_payload = c->fused_payload;
            }
            return actual_target;
        }
    }

    // Standard Dimensional Topologies
    if (actual_target == 0) {
        if (direction > 0) c->links[dim].first = 0;
        else c->links[dim].second = 0;
    } else {
        auto& t = matrix[actual_target];
        if (direction > 0) {
            c->links[dim].first = actual_target;
            if (t) t->links[dim].second = cell;
        } else {
            c->links[dim].second = actual_target;
            if (t) t->links[dim].first = cell;
        }
    }
    return actual_target;
}

// Extended Truthiness Evaluation
bool evaluate_truthiness(const CellValue& val) {
    if (std::holds_alternative<bool>(val)) return std::get<bool>(val);
    if (std::holds_alternative<double>(val)) return
std::get<double>(val) != 0.0;

    std::string s = std::get<std::string>(val);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch)
{ return std::tolower(ch); });
    if (s == "1" || s == "true" || s == "yes" || s == "on") return
true;
    if (s.empty() || s == "0" || s == "false" || s == "no" || s ==
"off") return false;
    return true;
}

// Payload Accessor: Slicing-Aware Dereference
CellValue get_cell_value(cell_id c_id, int64_t offset = 0, int64_t
length = -1) {
    auto it = matrix.find(c_id);
    if (it == matrix.end() || !it->second) return false;

    const CellValue& val = it->second->fused_payload ?
*(it->second->fused_payload) : it->second->primitive_value;
    if (std::holds_alternative<bool>(val)) return std::get<bool>(val);
    if (std::holds_alternative<double>(val)) return
std::get<double>(val);

    const std::string& src = std::get<std::string>(val);
    if (offset < 0 || static_cast<size_t>(offset) >= src.size())
return std::string("");
    size_t start = static_cast<size_t>(offset);
    size_t count = (length < 0) ? std::string::npos :
static_cast<size_t>(length);
    return src.substr(start, count);
}

// Payload Accessor: In-Place Mutation & Substring Patching
void set_cell_value(cell_id c_id, const CellValue& new_val, int64_t
offset = 0, int64_t length = -1) {
    auto it = matrix.find(c_id);
    if (it == matrix.end() || !it->second) return;

    CellValue& target = it->second->fused_payload ?
*(it->second->fused_payload) : it->second->primitive_value;

    if (std::holds_alternative<bool>(new_val) ||
std::holds_alternative<double>(new_val)) {
        target = new_val;
        return;
    }

    const std::string& input_str = std::get<std::string>(new_val);
    if (!std::holds_alternative<std::string>(target) ||
        (offset == 0 && (length < 0 || static_cast<size_t>(length) >=
std::get<std::string>(target).size()))) {
        target = input_str;
        return;
    }

    std::string& target_str = std::get<std::string>(target);
    size_t start = static_cast<size_t>(std::max<int64_t>(0, offset));
    if (start > target_str.size()) target_str.resize(start, ' ');

    size_t count = (length < 0) ? (target_str.size() - start) :
static_cast<size_t>(length);
    count = std::min(count, target_str.size() - start);
    target_str.replace(start, count, input_str);
}
```

______________________________________________________________________

## 3. The Dual-Wing Calling Convention & Spatial Parameter Binding

To eliminate hidden operand accumulators and register pollution, Vortex mandates a **Dual-Wing
Spatial Calling Topology**:

```
            [ Primary Out Target ] --- +d.step ---> [ Secondary Out Target ]
                    |
                    v -d.grab
            [ Opcode Node ]
                    |
                    v +d.grab
            [ Primary In Operand ] --- +d.step ---> [ Secondary In Operand ]
```

- **Outputs Wing (`-d.grab`)**: Output destination cells extend negward. If an operation yields
  multiple results (e.g., `#DIVMOD`), targets chain posward along `+d.step` from the primary out
  cell.
- **Inputs Wing (`+d.grab`)**: Positional parameters extend posward along `+d.grab` and chain
  sequentially posward along `+d.step`.
- **Snapshot-Before-Write Invariant**: The runtime snapshots all input payloads prior to writing to
  output cells, ensuring in-place operations (`#ADD out out out`) execute deterministically without
  memory aliasing corruption.

______________________________________________________________________

## 4. Cursor Associative Scopes: `d.vars` and `d.values`

Process cursors (Spin-Heads) maintain their own variable lookup environments directly on the spatial
matrix:

```
[ Spin-Head Cursor ]
        |
        v +d.vars
  [ Var Identifier: "lineage" ] --- +d.values ---> [ Root Manifold Node ]
        |                                                  |
        v +d.vars                                          +--- +d.child --> [ Person: "Isaac" ]
  [ Var Identifier: "counter" ] --- +d.values ---> [ 42.0 ]
```

- **Identifier Axis (`d.vars`)**: A linear rank of cells posward from the cursor holding variable
  name strings.
- **Storage Axis (`d.values`)**: Orthogonal links from each name cell pointing to its actual bound
  value.
- **Complex Data Containment**: The cell anchored along `+d.values` is not restricted to scalars; it
  may be the entry root of an arbitrarily deep multidimensional zzstructure (such as a tree, cyclic
  graph, or compiler AST).

______________________________________________________________________

## 5. First-Class Topological Logic Programming (Prolog Unification)

Vortex implements Robinson unification natively using zzstructure geometry, eliminating Warren
Abstract Machine (WAM) registers:

### Variable States in the Matrix

- **Unbound / Free Variable**: A variable cell whose `+d.values` link is 0 (null).
- **Bound Variable**: A variable cell with a posward link on `+d.values` targeting a ground literal
  or compound term manifold.
- **Aliased Variables**: Two or more variable cells unified together via `d.fuse`. Mutating one
  instantly binds the other.

### Backtracking & Topological Trail

Choice points and mutation logs exist entirely on the cursor's `+d.stack` dimension:

```
[ Spin-Head Cursor ]
        |
        v +d.stack
  [ Choice Point Metacell ]
        |
        +-- +d.warp --> [ Backtrack Alternate Branch Address ]
        |
        +-- +d.trail --> [ Trail Cell 1 ] --- +d.step ---> [ Trail Cell 2 ]
                                |                                   |
                          Target: Var_X                       Target: Var_Y
```

When a branch fails (`#FAIL`), the engine:

1. Walks the choice point's `+d.trail` rank.
1. Clears each recorded binding using `set_link(target, d_values, +1, 0)`.
1. Pops the frame from `+d.stack`.
1. Diverts cursor execution along `+d.warp`.

______________________________________________________________________

## 6. Full Verification Example: The `#ANCESTOR` Logic Engine

The following complete assembly track defines the ancestor logic rule over a family tree (Bob
$\leftarrow$ Charlie $\leftarrow$ Alice), runs an exhaustive search loop, logs every ancestor found,
and cleanly terminates when the query returns false:

```
;=============================================================================
; 1. BOOTSTRAP KNOWLEDGE BASE (Bob -> Charlie -> Alice along d.parent)
;=============================================================================
SET_LINK   0             d.parent  +1   -1        -> Cell_Bob
SET_VAL    Cell_Bob      "Bob"

SET_LINK   Cell_Bob      d.parent  -1   -1        -> Cell_Charlie
SET_VAL    Cell_Charlie  "Charlie"

SET_LINK   Cell_Charlie  d.parent  -1   -1        -> Cell_Alice
SET_VAL    Cell_Alice    "Alice"

;=============================================================================
; 2. LOGIC ROUTINE: #ANCESTOR
; Inputs:  +d.grab -> Slot_X (Target/Var), +d.step -> Slot_Y (Subject)
; Outputs: -d.grab -> Out_Result (Bound Node, Payload = true/false)
;=============================================================================
LABEL ANCESTOR_ENTRY
GET_LINK   cursor        d.stack   +1        -> top_choice
TEST_ZERO  top_choice
WARP_IF    top_choice    ANCESTOR_BACKTRACK

; --- First Invocation: Seed Traversal ---
GET_LINK   op_ancestor   d.grab    +1        -> slot_x
GET_LINK   slot_x        d.step    +1        -> slot_y
GET_LINK   slot_y        d.parent  +1        -> cur_parent
SET_LINK   op_ancestor   d.step    +1        cur_parent
WARP       ANCESTOR_LOOP

; --- Backtrack: Unwind Trail & Advance to Next Parent ---
LABEL ANCESTOR_BACKTRACK
GET_LINK   top_choice    d.trail   +1        -> trail_node
TEST_ZERO  trail_node
WARP_IF    trail_node    SKIP_TRAIL_UNWIND

GET_LINK   trail_node    d.grab    +1        -> bound_var
SET_LINK   bound_var     d.values  +1        0              ; Unbind variable
SET_LINK   top_choice    d.trail   +1        0

LABEL SKIP_TRAIL_UNWIND
GET_LINK   top_choice    d.grab    +1        -> saved_parent
GET_LINK   saved_parent  d.parent  +1        -> cur_parent  ; Step posward on d.parent

; Pop choice point frame
GET_LINK   top_choice    d.stack   +1        -> prev_stack
SET_LINK   cursor        d.stack   +1        prev_stack
SET_LINK   top_choice    d.stack   +1        0              ; Reclaim via GC

; --- Main Resolution Loop ---
LABEL ANCESTOR_LOOP
TEST_ZERO  cur_parent
WARP_IF    cur_parent    ANCESTOR_FAIL

; Push fresh choice point frame
SET_LINK   cursor        d.stack   +1   -1        -> choice_frame
SET_LINK   choice_frame  d.grab    +1        cur_parent

; Inspect Slot X binding status
GET_LINK   op_ancestor   d.grab    +1        -> slot_x
GET_LINK   slot_x        d.values  +1        -> x_val
TEST_ZERO  x_val
WARP_IF    x_val         UNIFY_FREE_VAR

; Ground Match: Slot X is already bound
GET_VAL    slot_x        val_x
GET_VAL    cur_parent    val_parent
EQ         val_x         val_parent     -> is_match
WARP_IF    is_match      ANCESTOR_SUCCEED
WARP       ANCESTOR_ADVANCE_NO_MATCH

; Unbound Unification: Bind slot_x to cur_parent
LABEL UNIFY_FREE_VAR
SET_LINK   slot_x        d.values  +1        cur_parent
SET_LINK   choice_frame  d.trail   +1   -1        -> trail_cell
SET_LINK   trail_cell    d.grab    +1        slot_x

; Emit Success
LABEL ANCESTOR_SUCCEED
GET_LINK   op_ancestor   d.grab    -1        -> out_slot
SET_LINK   out_slot      d.values  +1        cur_parent
SET_VAL    out_slot      true
WARP       ANCESTOR_RETURN

; Step past non-matching ground ancestor
LABEL ANCESTOR_ADVANCE_NO_MATCH
SET_LINK   cursor        d.stack   +1        0
GET_LINK   cur_parent    d.parent  +1        -> cur_parent
WARP       ANCESTOR_LOOP

; Exhausted Ancestry
LABEL ANCESTOR_FAIL
GET_LINK   op_ancestor   d.grab    -1        -> out_slot
SET_LINK   out_slot      d.values  +1        0
SET_VAL    out_slot      false

LABEL ANCESTOR_RETURN
; Return along cursor +d.spin back to caller

;=============================================================================
; 3. CALLER LOOP (Exhaustive Solution Logging, Breaks on False)
;=============================================================================
LABEL CALLER_ENTRY
; Allocate Opcode invocation context
SET_LINK   cursor        d.step    +1   -1        -> op_ancestor
SET_VAL    op_ancestor   "#ANCESTOR"

; Allocate Out Parameter Slot (-d.grab)
SET_LINK   op_ancestor   d.grab    -1   -1        -> out_res
SET_VAL    out_res       false

; Allocate Free Variable X (+d.grab)
SET_LINK   op_ancestor   d.grab    +1   -1        -> var_x
SET_VAL    var_x         "Var_X"

; Wire Subject Alice (+d.step from Var_X)
SET_LINK   var_x         d.step    +1        Cell_Alice

LOG_LITERAL "Querying all ancestors of Alice:"

LABEL CALLER_LOOP_HEAD
SET_LINK   cursor        d.spin    +1        ANCESTOR_ENTRY ; Invoke routine

; Evaluate result
GET_VAL    out_res                                -> res_status
TEST_TRUTHY res_status
WARP_IF    res_status    PROCESS_RESULT
WARP       CALLER_LOOP_EXIT

LABEL PROCESS_RESULT
GET_LINK   out_res       d.values  +1        -> bound_node
GET_VAL    bound_node                             -> ancestor_name

LOG_TOKEN  "Found Ancestor: "
LOG_LINE   ancestor_name

; Trigger backtracking on next loop iteration
WARP       CALLER_LOOP_HEAD

LABEL CALLER_LOOP_EXIT
LOG_LITERAL "Search complete: #ANCESTOR returned false."
SET_VAL    cursor        "HALT"
```
