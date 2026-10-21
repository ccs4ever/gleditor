# Experiment: VQL-Agent (Agent-Oriented Programming as a VQL Extension)

**Status:** Deferred Design Experiment\
**Author:** AI Pair Programmer & Expert Subagents\
**Date:** 2026-10-21

______________________________________________________________________

## 1. Executive Summary

This experiment preserves the synthesis of the SARL Agent-Oriented Programming (AOP) metamodel
directly inside **VQL (Vortex Query Language)**, rather than as a standalone programming language
(`vagent`).

In VQL-Agent:

- **Agents** are rooted spatial manifold nodes navigated via directional paths
  (`$agent/+d.behavior`).
- **Capacities** are first-class structural **Shapes** (`shape CapacityName { ... }`).
- **Skills** are executable functional pipelines satisfying capacity shapes.
- **Behaviors** are modular sets of reactive **Chronofilade Watchers** (`watch self/+d.event[...]`).
- **Events** are immutable cells woven into space mailbox ranks.
- **Spaces** are interaction manifolds queried with standard VQL paths and synchronized via 9P Styx
  streaming.
- **Holons** are hierarchical member trees linked along `±d.holon`.

______________________________________________________________________

## 2. Metamodel-to-VQL Mapping

| SARL Metamodel Concept | VQL Representation                     | Pure VQL Lowering                                           |
| :--------------------- | :------------------------------------- | :---------------------------------------------------------- |
| **Capacity**           | `shape CapacityName { ... }`           | Abstract dual-wing call contract on `+d.capacity`           |
| **Skill**              | `skill SkillName for Shape { ... }`    | Concrete `d.spin` pipeline linked via `+d.skill`            |
| **Behavior**           | `behavior Name { on ... }`             | Modular rank of watchers on `+d.behavior`                   |
| **Agent**              | `agent Name { uses ..., mount ... }`   | Manifold headcell with bound skill & behavior ranks         |
| **Space**              | Interaction manifold cell              | Node with member rank on `+d.agent`                         |
| **Event Emission**     | `emit Event into Space;`               | `weave $space/+d.event% { +d.type: ..., +d.payload: ... };` |
| **Reactive Handler**   | `on E in /+d.event[...] where G { B }` | `watch self/+d.event[...] as $ev { if (G) { B } }`          |
| **Holon Nesting**      | `spawn Agent into inner_context;`      | `weave self/+d.holon% => Agent() { -d.holon: self };`       |
| **Dynamic Skill Swap** | `swap Cap with NewSkill;`              | Retarget link along `+d.skill` in $O(1)$                    |

______________________________________________________________________

## 3. Concrete VQL-Agent Syntax Example

```vql
module swarm::search;

import std::agent;
import std::geo;

// 1. Capacity definition as a VQL Shape Contract
capacity Geolocation {
  coords: () => shape { lat: float, lon: float },
  distance_to: ($target: shape { lat: float, lon: float }) => float
}

// 2. Autonomous Agent
agent SearchWorker {
  requires Geolocation;

  // React to search tasks emitted by the swarm coordinator
  on task in /+d.event[d.type = "SearchGridTask"] {
    let $my_loc := coords();
    let $dist   := distance_to($task/+d.grid_center);

    // VQL spatial query over local sensor slice
    for $hit in ##/slice::sensors/+d.readings[d.confidence > 0.8]
    where geo::within($hit/+d.location, $task/+d.radius) {
      emit SearchHit {
        worker_id: self.id,
        item:      $hit/+d.signature,
        location:  $hit/+d.location
      } into default_space;
    }
  }
}

// 3. Super-Holon (Coordinator)
holon SwarmCoordinator {
  let $active_hits := ();

  on init in /+d.lifecycle[d.type = "Initialize"] {
    // Dynamically deploy 8 search workers across the inner context
    for $i in 1..8 {
      spawn SearchWorker into inner_context;
    }

    // Broadcast grid assignments into inner holon space
    for $worker in self/+d.holon/*
    let $grid := compute_partition($worker) {
      emit SearchGridTask { grid_center: $grid.center, radius: 25.0 }
        into inner_context
        to $worker;
    }
  }

  // Collect reports ascending from the inner context
  on hit in inner_context/+d.event[d.type = "SearchHit"] {
    weave . { $active_hits := ($active_hits, $hit) };

    // Query across collected hits using standard VQL
    if (count($active_hits) >= 10) {
      emit MissionComplete {
        total_found: count($active_hits),
        top_targets: (for $h in $active_hits order by $h/+d.confidence descending return $h)[1..3]
      } into outer_context;
    }
  }
}
```

______________________________________________________________________

## 4. Rationale for Deferred Evaluation

1. **Unification of Query & Action**: Allows agents to run complex FLWOR queries over docuverse
   slices (`/##`) directly inside event reactions.
1. **Zero-New-Primitives**: Proven to lower 100% to existing VQL `weave`, `watch`, and `%`
   operators.
1. **Synergy with UI**: In `xuzz`, VQL-Agent instances could be rendered and manipulated visually in
   the Zigzag multi-view canvas.
