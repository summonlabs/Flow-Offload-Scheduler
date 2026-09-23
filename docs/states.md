# States, generations and durable layout

## Lifecycles

**Target lifecycle** — `Unknown`, `Discovered`, `Ready`, `Degraded`,
`Draining`, `Quiesced`, `Removed`. Only `Ready` accepts new flows. A target
that has not been declared in the current coordinator incarnation is
`Unknown` regardless of what was persisted, so a restart cannot resurrect
liveness.

**Effect state** — `Unknown`, `IntentIssued`, `Acknowledged`, `Applied`,
`Verified`. Only `Verified` is durable across a restart.

**Migration phase** — `Planned`, `IntentIssued`, `SourceAcknowledged`,
`DestinationAcknowledged`, `EffectApplied`, `EffectVerified`, `Completed`,
`Aborted`, `Fenced`. `Fenced` is terminal and means "the coordinator
incarnation that authorised this ended"; it can never be resumed.

**Evidence state** — `Unknown`, `Known`, `Unsupported`, `Conflicting`,
`Stale`. Only `Known` can justify a decision, and only while fresh.

## Generations and epochs

| Value | Advances when | Effect |
|---|---|---|
| `FlowGeneration` | The flow's processing is redefined | Releases the previous placement and fences its attempts |
| `TargetIncarnation` | A target is replaced | Aborts attempts touching the old incarnation; assignments keep identity but lose their effect state |
| `CapabilityGeneration` | A target's capabilities change | Invalidates evidence and recommendations from the previous generation |
| `TopologyGeneration` | The topology view advances | Every target declared under an older generation becomes not live |
| `PolicyGeneration` | Policy changes | Invalidates authorizations issued under the previous policy |
| `ScheduleGeneration` | A placement request changes the schedule | Invalidates recommendations and authorizations from the previous schedule |
| `CoordinatorEpoch` | Every restart | Fences all pre-restart authority |
| `BootId` | Every restart | Distinguishes two incarnations that could observe the same epoch |

## Durable layout

`state.snapshot`

```
 0   8  magic "FOSSNP01"
 8   4  container version
12   4  semantics version
16   4  flags
20   4  reserved (must be zero)
24   8  coordinator epoch
32   8  boot.high
40   8  boot.low
48   8  payload length
56   8  commit sequence
64  32  payload digest (SHA-256)
96  32  header digest (SHA-256 over bytes 0..95)
128  N  canonical payload
```

`state.journal`

```
 0   8  magic "FOSJNL01"
 8   4  container version
12   4  semantics version
16   8  base commit sequence
24  32  base payload digest
56   8  reserved (must be zero)
64  32  header digest (SHA-256 over bytes 0..63)
96   ...  records
```

```
record:  0  4  payload length
         4  8  record sequence
        12  1  record kind (1 = full state)
        13  3  reserved (must be zero)
        16  N  canonical payload
      16+N 32  digest over bytes 0..(15+N)
```

Commit order: append and flush the journal record, then optionally compact
(snapshot through a temporary file and an atomic rename, then rewrite the journal
header in place). A commit is acknowledged only after the record has reached the
device, so a crash can lose the in-flight commit but never reports success for
one that did not land.

Recovery skips journal records whose sequence does not exceed the snapshot's, so
a crash between the snapshot rename and the journal rewrite is idempotent rather
than a regression.

## What is deliberately not durable

* Live load evidence. Observations must be re-supplied so that a stale one can
  never look current.
* Target liveness.
* Authorizations. They are bound to one epoch and one boot, so persisting them
  would create exactly the replay hazard they exist to prevent.
* Recommendations. Only their identifier high-water mark is implied by the
  in-memory counter, which restarts at one; a stale identifier resolves to
  "unknown", never to a stale grant.
