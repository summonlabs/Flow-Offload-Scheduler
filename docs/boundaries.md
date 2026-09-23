# Systems boundary

This document states, precisely, what this runtime owns and what it refuses to
absorb. Absorbing a neighbour would make the runtime a different product.

## Owned

| Concern | Interface |
|---|---|
| Host-versus-offload eligibility | `Engine::place` |
| Stable target ranking | `CandidateView::rank`, deterministic rank tuple |
| Policy constraint evaluation | `Engine::set_policy`, `Policy` |
| Capacity-aware placement | `CapacityVector`, conservation accounting |
| Sticky and stateful affinity | `Policy::sticky`, `ExclusiveStateKeyId` |
| Migration planning and safety | `MigrationContract`, `Engine::issue_intent` |
| Explicit fallback | `FallbackPolicy`, `AcceptedFallbackToHost/Offload` |
| Bounded rebalance | `Engine::rebalance`, `RebalanceReport` |
| Authority and fencing | `Authorization`, `FenceRecord`, `CoordinatorEpoch` |
| Deterministic explanation | `Explanation`, `render` |
| Canonical export | `export_json`, `export_binary`, `export_text` |

## Not owned, by construction

* **Packet forwarding.** No packet path exists. The runtime has no notion of a
  packet beyond a capacity dimension.
* **Device programming.** No device handle is opened. Targets are typed
  declarations supplied by their owner.
* **QoS and congestion control.** Not modelled, not scheduled, not enforced.
* **Topology discovery.** Topology arrives as a `TopologyGeneration` and as
  target declarations. The runtime never probes anything.
* **Telemetry collection.** Load evidence is pushed in. There is no pull path,
  no counter scrape, no sampling loop.
* **Policy authorship.** The runtime evaluates a policy generation. It never
  chooses its own constraints.

## Inputs it treats as evidence

Inputs carry provenance (`EvidenceSourceId`, `EvidenceSequence`) and time
(`Timestamp`). Acceptance rules:

* Evidence for an undeclared target is refused; evidence never creates a target.
* Evidence whose incarnation or capability generation does not match the current
  declaration is refused.
* A sequence below the per-source watermark is a replay and is refused.
* A repeated sequence with different content is a conflicting duplicate.
* Two sources disagreeing at the same instant make the load unknown, not
  averaged, and the conflict is counted.
* An observation older than the freshness horizon at the evaluation instant is
  stale and cannot justify a decision.

## Outputs it produces

Placement decisions, recommendations, authorizations, migration intents,
acknowledgement records, applied-effect records, verified-effect records,
explanations, and canonical exports. Nothing in that list is a side effect on the
network.
