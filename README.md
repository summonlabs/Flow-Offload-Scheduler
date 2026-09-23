# Flow Offload Scheduler

A standalone, vendor-neutral C++20 runtime that decides **where flow processing
executes** — in host networking or on a NIC / SmartNIC / DPU execution target —
and that never does anything else.

The runtime is a *placement authority*: it consumes flows, execution targets,
capability and load evidence, policy, and authority inputs; it produces
deterministic placement recommendations, authorizations, migration intents, and
verified-effect records. It is the single owner of that decision and of nothing
adjacent to it.

* Library version: 1.0.0
* Persisted state semantics version: 1
* Wire protocol version: 1
* Export schema version: 1

---

## 1. Exact systems boundary

### What this runtime owns

* Deterministic host-versus-offload eligibility for a flow processing function.
* Stable target ranking with total, reproducible tie-breaking.
* Policy constraint evaluation, capacity-aware placement, sticky and stateful
  affinity, explicit fallback, bounded rebalance.
* Authority: recommendations, authorizations, migration intents, fences.
* The lifecycle that separates recommendation, authorization, migration intent,
  acknowledgement, applied effect, and verified effect.
* Versioned, integrity-checked persistence of its own state, restart fencing,
  and canonical explanations and exports.

### What this runtime explicitly does not do

* It does not forward, drop, rewrite, shape or queue packets.
* It does not program a NIC, SmartNIC, DPU, ASIC or switch.
* It does not implement QoS or congestion control.
* It does not discover topology.
* It does not collect telemetry.
* It does not decide policy; it evaluates a policy generation supplied to it.

Neighbouring runtimes supply topology, observations, capability evidence, policy,
authority, and execution/effect reports. Those enter through the ingest surface
and leave through the decision surface. Nothing adjacent is absorbed: for
example, load evidence is an input, and this runtime never polls a device for it.

---

## 2. Submission is not completion

Every stage of the decision is a distinct, separately typed record, and each one
is required before the next is accepted:

| Stage | Record | Meaning |
|---|---|---|
| Recommendation | `Recommendation` | Advice. Carries no authority. |
| Authorization | `Authorization` | Authority for exactly one recommendation, one coordinator epoch, one boot. Never durable. |
| Migration intent | `MigrationIntent` | A fenced attempt to move processing, with the contract that makes it legal. |
| Acknowledgement | `MigrationAcknowledgement` | An executor says it will act. Not an effect. |
| Applied effect | `AppliedEffect` | An executor reports what it installed, with a digest of the state. |
| Verified effect | `EffectObservation` | An independent observation that matches the applied digest. Only this completes the move. |

A recommendation alone cannot create an intent (`RejectedAuthorizationMissing`).
An acknowledgement cannot stand in for an application
(`RejectedMigrationEffectBeforeAck`). An observation with a different digest
cannot verify an effect (`RejectedMigrationVerificationMismatch`).

---

## 3. Core state model

Strongly typed identities, each an incompatible type at compile time:

| Concept | Type |
|---|---|
| Flow | `FlowId` |
| Flow generation | `FlowGeneration` |
| Processing function | `ProcessingFunctionId` |
| Host | `HostId` |
| Device | `DeviceId` |
| Execution target | `TargetId` |
| Target incarnation | `TargetIncarnation` |
| Capability generation | `CapabilityGeneration` |
| Topology generation | `TopologyGeneration` |
| Policy generation | `PolicyGeneration` |
| Schedule generation | `ScheduleGeneration` |
| Coordinator epoch | `CoordinatorEpoch` |
| Coordinator boot | `BootId` (128-bit) |
| Migration attempt | `MigrationAttemptId` |
| Fence | `FenceId` |
| Exclusive flow state | `ExclusiveStateKeyId` |
| Assignment, recommendation, authorization, request, contract | `AssignmentId`, `RecommendationId`, `AuthorizationId`, `RequestId`, `MigrationContractId` |

Semantics are never stringly typed. Lifecycle (`TargetLifecycle`), statefulness
(`Statefulness`), cost class (`CostClass`), evidence quality
(`EvidenceState`), effect progress (`EffectState`), migration phase
(`MigrationPhase`) and load model (`LoadModel`) are enumerations. Unit-bearing
quantities are `CapacityVector` (packets per second, bytes per second, state
bytes, table entries) and `Timestamp`/`Duration`. Authority, generations and
compatibility are compared as typed values, never as text.

**Unknown, stale, conflicting, incomplete, invalid and unsupported are distinct
states.** An `Unknown` enumerator never compares equal to a known value, and a
missing observation never becomes zero:

* No load evidence for an eligible target yields `RejectedMissingEvidence`, not
  "idle".
* A zero-capacity dimension makes utilisation *undefined*
  (`RejectedCapacityUnknown`), not zero.
* An `Unknown` cost class never satisfies a cost bound.
* A capability bit this runtime does not define is refused
  (`RejectedUnknownCapability`) rather than ignored.

---

## 4. Determinism contract

For a fixed set of flows, targets, accepted evidence, policy, and evaluation
instant, the decision is a pure function of those inputs:

* No wall clock is ever read. Every decision takes an explicit
  `evaluation_instant` supplied by the caller.
* No hash container participates in a decision; internal state is held in
  ordered maps keyed by typed identity.
* No pointer value, thread identity, or scheduling order influences a result.
* Collections are canonicalised (ascending typed identity) before evaluation, so
  input order cannot change an outcome.
* Ranking is a lexicographic comparison of
  `(sticky, domain preference, cost class, utilisation after placement, target
  identity)`. The final component is unique, so the order is total and there are
  no ties to break arbitrarily.
* Canonical serialization is big-endian, fixed-width and length-delimited, with
  collections in canonical order. The same state always produces byte-identical
  output and the same SHA-256 digest.

This is asserted by property tests that run the same scenario with forward and
reversed declaration order, three times over, across dozens of seeded random
scenarios, and by concurrency tests that compare a serial run against a
multi-threaded run.

One part of the state is deliberately outside the determinism claim: the
monotonic identifier counters (`AssignmentId`, `RecommendationId`,
`AuthorizationId`, `MigrationAttemptId`, `FenceId`) are allocated in the order
in which operations commit. Under concurrent submission that order is
scheduling-dependent, so the *mapping from flow to identifier* can differ between
two concurrent runs.

A second, stronger boundary applies to concurrency itself. Independent requests
submitted concurrently are serialised by the engine lock, and each one is
deterministic given the state it observes, but the interleaving is chosen by the
scheduler. Two concurrent runs of the same set of independent requests may
therefore reach different - equally valid - schedules, because an earlier
committed placement changes the load the next request observes. Byte-identity is
claimed for sequential execution, including every batch inside a single request,
and that is what the property tests assert. For concurrent execution the tests
assert validity instead: every operation completes, every flow is placed exactly
once, every placement matches its target's current incarnation and capability
generation, identifiers are unique and confined to `1..N`, and demand is
conserved.

---

## 5. Architecture

```
include/flow_offload/     public headers (installed)
  identity.hpp            strongly typed identities and generations
  reason.hpp              stable numeric reason codes and Status
  units.hpp               typed time, checked arithmetic, capacity vectors
  model.hpp               flows, targets, evidence, policy, contracts, decisions
  limits.hpp              every bound enforced before allocation
  canonical.hpp           canonical encoder/decoder with checked reads
  digest.hpp              self-contained SHA-256
  serialize.hpp           validated codecs for every persisted record
  state.hpp               durable state and counters
  store.hpp               versioned, integrity-checked, crash-safe store
  thread_pool.hpp         bounded worker pool
  engine.hpp              the placement authority
  export.hpp              canonical JSON / binary / text export
  protocol.hpp            framed bounded wire protocol
  service.hpp             service and client
src/                      implementation
tools/fos_ctl.cpp         inspection and control CLI
tools/fos_schedd.cpp      service daemon
tests/                    unit, integration, property, concurrency, process tests
examples/downstream/      independent find_package consumer
```

The public surface is a narrow library API. There is no third-party dependency:
SHA-256, the canonical codec, the framing, the sockets, the worker pool and the
test harness are all in-tree.

---

## 6. Using the library

```cpp
#include "flow_offload/engine.hpp"

flow_offload::Engine engine;                 // in-memory
flow_offload::EngineConfig config;           // or durable:
config.persistence_enabled = true;
config.store.directory = "/var/lib/fos";
flow_offload::Engine persistent(config);

engine.open();
engine.set_policy(policy);                   // policy generation 1
engine.declare_target(host_descriptor);      // host packet path
engine.declare_target(smartnic_descriptor);  // SmartNIC execution target
engine.register_flow(flow_descriptor);
engine.ingest_load(load_evidence, instant);

flow_offload::PlacementRequest request;
request.request = flow_offload::RequestId{1};
request.evaluation_instant = flow_offload::Timestamp{2000};
request.items.push_back({flow_offload::FlowId{1}, flow_offload::FlowGeneration{}});

const flow_offload::PlacementResult result = engine.place(request);
// result.placements[0].outcome, .target, .explanation
```

Decision-stage calls:

```cpp
flow_offload::Authorization authorization;
engine.authorize(result.placements[0].recommendation, instant, authorization);

flow_offload::MigrationIntent intent;
intent.flow = flow_offload::FlowId{1};
intent.destination_target = flow_offload::TargetId{2};
/* destination incarnation and capability generation */
engine.issue_intent(authorization, intent);   // needs a contract when stateful

engine.acknowledge(source_ack);               // executor says it will act
engine.acknowledge(destination_ack);
engine.report_applied(applied_effect);        // executor reports what it installed
engine.observe(effect_observation);           // independent verification
```

### Safe migration of stateful processing

A stateful flow may never be migrated without an explicit migration contract that
covers the flow generation, the destination incarnation and capability
generation, and the current policy generation, and that declares state transfer,
ordering preservation, rollback, and exclusive handoff. Anything less is refused
with `RejectedMigrationContractMissing`, `RejectedStatefulMigrationUnsafe` or
`RejectedMigrationContractMismatch`.

An exclusive state key has at most one holder at any time. A second attempt for
the same flow is refused with `RejectedMigrationAlreadyInFlight`, and a second
flow claiming a live key is refused with `RejectedExclusiveStateConflict`.

---

## 7. Building, testing, installing

```
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /path/to/prefix
```

Options:

* `FLOW_OFFLOAD_SCHEDULER_BUILD_TOOLS` (default `ON`) — build `fos_ctl` and `fos_schedd`.
* `FLOW_OFFLOAD_SCHEDULER_BUILD_TESTS` (default follows `BUILD_TESTING`).
* `BUILD_SHARED_LIBS` — the library builds as a static or shared library.

The installed package exports `Summon::FlowOffloadScheduler`. First-party
warning flags are applied directly to each target and are not part of the export,
so they never reach a consumer.

```cmake
find_package(FlowOffloadScheduler 1.0 REQUIRED CONFIG)
target_link_libraries(consumer PRIVATE Summon::FlowOffloadScheduler)
```

`examples/downstream` is a complete, independent consumer that uses installed
headers and the exported target only.

---

## 8. Command line

```
fos_ctl [--store <dir>] <command> [options]

status              coordinator identity, generations, recovery, counters
verify              re-read the store and classify its integrity
export              canonical export (--format json|text|binary)
declare-target      declare or re-declare an execution target
register-flow       register a flow processing request
ingest-load         supply target load evidence
set-policy          install a policy generation
register-contract   register a safe migration contract
place               compute a placement decision
explain             explain the committed placement of a flow
rebalance           plan bounded rebalance migrations
begin-migration     authorise and issue one migration intent
complete-migration  acknowledge, apply and verify a migration attempt
seed-demo           build a deterministic demonstration scenario
```

`--crash-at <boundary>` hard-kills the process at a named durable boundary
(`before-journal-append`, `after-journal-append-before-flush`,
`after-journal-flush-before-snapshot`, `after-snapshot-write-before-rename`,
`after-snapshot-rename-before-journal-rewrite`,
`after-journal-rewrite-before-acknowledge`). It exists so crash behaviour is
proved against a real process death instead of a simulated exception.

`fos_schedd [--store <dir>] [--bind <addr>] [--port <n>] [--workers <n>]`
serves the protocol and prints `READY <port>` on standard output once bound.

---

## 9. Service protocol

Framed, bounded, integrity-checked, and correlation-matched:

```
 0  4  payload length        (excludes this header and the trailing digest)
 4  2  protocol version
 6  1  operation
 7  1  flags
 8  8  correlation identifier
16  4  status reason code
20  4  reserved, must be zero
24  N  payload
24+N 32 SHA-256 over bytes [0, 24+N)
```

Operations: `hello`, `declare-target`, `register-flow`, `ingest-load`,
`set-policy`, `register-contract`, `place`, `rebalance`, `explain`,
`export`, `shutdown`.

A frame is refused with a distinct reason code for a truncated length, an
oversized length, a wrong version, a non-zero reserved field, an unknown
operation, an undefined reason code, and a digest mismatch. The service owns no
scheduling state: every decision is taken by the same `Engine` the library
exposes, so the transport cannot diverge from the library.

---

## 10. Persistence and restart

A store directory holds:

* `state.snapshot` — a 128-byte integrity-checked header plus a canonical state
  payload, committed atomically through a temporary file and a rename.
* `state.journal` — a 96-byte header plus append-only full-state commit records,
  each with its own digest, flushed to the device before the commit is
  acknowledged.

Recovery classifications: `clean-reopen`, `torn-tail-truncated`,
`journal-replayed`, `snapshot-only`, `journal-only`, `empty-store`,
`corrupt`, `incompatible-version`, `semantics-mismatch`, `oversized`,
`unavailable`. A torn tail is truncated to the last intact record; a damaged
record with intact records after it is mid-file corruption and is refused
outright. A store that cannot be opened refuses to open — it never silently
becomes an empty store.

On restart the runtime:

* advances the coordinator epoch and derives a new boot identity;
* fences every attempt that was in flight, permanently
  (`RejectedFencedByRestart`);
* marks **every** target as not live, so liveness must be re-established by its
  owner before anything can be placed;
* discards live load evidence — the persisted evidence watermarks still refuse a
  replayed or older sequence;
* demotes any assignment whose effect was not durably verified to `Unknown`;
* never persists authorizations at all, so pre-restart authority cannot be
  replayed by construction.

---

## 11. Proof surface

Invariants asserted by the test suite:

| Invariant | Where |
|---|---|
| Capacity conservation: committed demand per target never exceeds its capacity, and the sum over targets equals the demand of each placed flow exactly once | `test_property`, `test_placement` |
| Order independence: reversing declaration order changes nothing | `test_property`, `test_placement` |
| Stable tie-breaking: equal candidates resolve by target identity | `test_placement` |
| No placement on an unsupported target (capabilities, function, statefulness, cost, lifecycle) | `test_placement`, `test_property` |
| No stale-generation reuse: incarnations, capability generations, topology generations, policy generations and schedule generations are all checked | `test_placement`, `test_property` |
| No double assignment of exclusive flow state | `test_migration`, `test_property` |
| Deterministic fallback | `test_property`, `test_placement` |
| Restart fencing | `test_persistence`, `test_process` |
| Accepted state is deterministic from accepted evidence and policy | `test_property` |
| Stale or superseded evidence cannot justify current authority | `test_placement`, `test_migration` |
| Missing evidence never becomes false, zero or success | `test_placement`, `test_core` |
| Duplicate delivery is idempotent or explicitly fenced | `test_placement`, `test_migration`, `test_transport` |
| Every bounded truncation, refusal and eviction is observable and accounted for | `test_placement`, `test_persistence` |
| Persistence round-trips all correctness-critical state without semantic loss | `test_persistence` |
| Conservative restart does not resurrect liveness or authority | `test_persistence`, `test_process` |
| Malformed, corrupt, truncated and oversized input cannot produce valid-looking success | `test_core`, `test_property`, `test_transport`, `test_process` |

Adversarial coverage includes rapid capability churn, duplicate and reordered
evidence, target disappearance mid-migration, conflicting authority, integer
extremes, absurd sizes, partial writes, journal damage, cancellation at awkward
boundaries, repeated open/close, real process death, and concurrent
ingest/query/cancel.

---

## 12. REAL / SYNTHETIC / UNSUPPORTED

**REAL — executed and verified in this repository**

* The C++20 library, CLI and service daemon, built and tested with MSVC 19.44
  (`/W4 /WX /permissive-`) in Debug and Release.
* Real TCP sockets on the loopback interface, driven by independent OS
  processes, with real process spawn, real hard kill, and real restart against
  the same durable store.
* Real file-system persistence: real snapshots, real append-only journals, real
  fsync/`_commit`, real atomic renames, real torn tails and real corruption.
* Real concurrency: multiple threads and multiple clients exercising one engine.
* Real SHA-256, verified against the published FIPS 180-4 test vectors.
* Real CMake package export and an independent `find_package` consumer built
  against the installed tree only.

**SYNTHETIC — fixtures, not hardware**

Every execution target in the test suite is a **SYNTHETIC** descriptor: a
typed record constructed in a test or by `fos_ctl seed-demo`. No NIC,
SmartNIC, DPU or switch is contacted, programmed or observed anywhere in this
repository. "Offload device" targets are declarations supplied by a caller;
the runtime treats them as input and would behave identically for any other
caller-supplied target. Load evidence in the tests is likewise synthetic.

The runtime is designed so that real hardware evidence can be supplied through
the same ingest surface without changing a single decision rule — but that
integration is not part of this repository and is not claimed.

**UNSUPPORTED — not available here, and not claimed**

* **AddressSanitizer / UndefinedBehaviorSanitizer**: the MSVC installation
  available for this work does not include the x64 ASan runtime
  (`clang_rt.asan_dynamic-x86_64.dll`/`.lib` are absent; only
  `asan_compat.lib` is present), so `/fsanitize=address` cannot link. No
  sanitizer result is claimed. The compensating measures actually used are the
  Debug CRT with iterator debugging enabled, `/RTC1`, `/sdl`, `/W4 /WX`,
  exhaustive bounds-checked decoding, and adversarial input tests.
* **GCC / Clang builds**: only MSVC was used. The CMake project carries GNU-style
  warning flags for those compilers, but no GCC or Clang build or test run is
  claimed.
* **POSIX builds**: the socket layer and file handling contain POSIX paths, but
  only the Windows paths were built and exercised.
* **Vendor hardware or vendor protocols**: NIC, SmartNIC, DPU, ASIC, RDMA,
  InfiniBand, NVLink, CUDA, and multi-host fabrics are not exercised.
* **Large-scale performance**: the benchmarks measure completed placements on one
  machine and are not a capacity claim.

---

## 13. Validation performed

* Debug and Release builds, both with `/W4 /WX /permissive-`, zero warnings.
* Full CTest suite: unit, integration, property, concurrency, transport, and
  independent-process tests.
* Real multiprocess proof: `fos_schedd` launched as a separate OS process,
  driven over loopback TCP, hard-killed, restarted, and re-verified.
* Hard kills injected at six durable boundaries, with the resulting store
  verified by a separate process.
* Fresh-clone closure from committed sources and from the pushed annotated tag:
  configure, build, test, install, and run the downstream `find_package`
  consumer.

---

## 14. Genuine limitations

* **Journal write amplification.** A commit record carries the whole durable
  state, so the cost of a commit grows with the size of the state. Compaction
  bounds the journal's size but not the per-commit cost. A delta journal would be
  the next step.
* **The durable commit happens under the engine lock.** Correctness is
  straightforward and the ordering is easy to reason about, but a slow device
  serialises mutating calls. Read-only queries are unaffected.
* **Evidence is not durable.** Load evidence must be re-supplied after a restart.
  This is deliberate — a stale observation must never look current — and it means
  a restarted runtime refuses to place until its inputs are re-declared.
* **A newer topology generation invalidates older declarations.** Correct but
  blunt: a target declared under an older generation must be re-declared.
* **Rebalance planning does not adjust the source target's projected load.** The
  improvement estimate for each flow is computed against the pre-move state, so a
  large batch can be slightly optimistic about the destination's utilisation. The
  capacity check at apply time is exact, so this affects ordering preference, not
  safety.
* **Bounds are compile-time constants**, not runtime-configurable per engine.
* **One store directory per engine**, with no multi-writer coordination. Two
  coordinators pointed at the same directory would fence each other's authority
  rather than cooperate.
* **Recommendations and authorizations are in-memory only** and bounded by
  `max_history`; exceeding the bound evicts the oldest and counts the eviction.
  A caller that keeps a recommendation id past that bound gets
  `RejectedRecommendationMismatch` rather than a stale grant.

---

## License

Apache License 2.0. Copyright 2026 Summon Software Labs. No telemetry transmission.
