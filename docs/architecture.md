# Architecture

## Layering

```
 callers / neighbouring runtimes
        |  (topology, observations, capability evidence, policy, authority,
        |   execution and effect reports)
        v
 +---------------------------------------------------------------+
 | Flow Offload Scheduler                                        |
 |                                                               |
 |  ingest        flows, targets, load evidence, policy,          |
 |                migration contracts, topology generations       |
 |                                                               |
 |  decision      eligibility -> ranking -> capacity -> fallback  |
 |                (pure function of the accepted inputs)          |
 |                                                               |
 |  authority     recommendations, authorizations, fences         |
 |                                                               |
 |  lifecycle     intents, acknowledgements, applied effects,     |
 |                verified effects                                |
 |                                                               |
 |  persistence   snapshot + append-only journal, recovery        |
 |  transport     framed bounded protocol over TCP                |
 +---------------------------------------------------------------+
        |
        v
 placement decisions, migration intents, verified-effect records,
 canonical explanations and exports
```

The runtime never calls outward. There is no polling, no callback into a device,
no telemetry emission, and no hidden clock. Every input arrives through an
explicit ingest call or a protocol frame.

## Decision procedure

1. **Validate the request.** Request identity, evaluation instant, policy and
   schedule generation expectations, size bounds, duplicate items. A malformed or
   ambiguous request is refused with a stable reason code and changes nothing.
2. **Deduplicate.** A repeated request identifier with the same canonical digest
   is idempotent; the same identifier with a different digest is a conflict.
3. **Canonicalise.** Items are ordered by typed identity, so the outcome cannot
   depend on input order.
4. **Evaluate every target** for every item. Eligibility is a conjunction of
   liveness, domain policy, forbidden host/device lists, function policy,
   capability satisfaction, statefulness support, cost bound, locality, evidence
   availability and freshness, and capacity including the policy reserve.
   A failure yields one stable reason code per (flow, target) pair.
5. **Rank** the eligible candidates by
   `(sticky, domain preference, cost class, utilisation after placement, target
   identity)`. The last component is unique, so the order is total.
6. **Apply the fallback policy** when the best candidate is outside the preferred
   domain. Fallback is explicit, recorded, and counted.
7. **Plan the whole batch first**, tracking planned and released demand so that
   capacity is conserved across the batch, and only then **commit**: publish one
   schedule generation, create assignments, create one recommendation per
   accepted decision.
8. **Persist** before returning, so an acknowledged decision is durable.

## Concurrency model

* The engine holds one mutex over its state. Read and write paths both take it,
  so a query always sees a consistent state.
* The asynchronous path uses a bounded worker pool. Publishing an operation's
  result never happens under the state lock: the worker computes with the state
  lock held, releases it, and only then takes the operation lock.
* The two locks are never held simultaneously, so there is no lock order to
  invert.
* The pool is joined only with no engine lock held, because workers take the
  engine lock themselves.
* Shutdown closes sockets to unblock blocked reads rather than waiting for a
  peer.

## Persistence

See `docs/states.md` for the record layout and `README.md` section 10 for the
recovery classifications.
