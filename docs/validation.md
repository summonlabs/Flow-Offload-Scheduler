# Validation

## Build configurations

| Configuration | Flags |
|---|---|
| Debug | `/W4 /WX /permissive- /utf-8 /Zc:__cplusplus /EHsc /sdl`, `/Od /RTC1`, iterator debugging |
| Release | same warning policy, `/O2` |

Both configurations build every target with zero warnings. The warning flags are
applied per target and are not exported, so a consumer never inherits them.

## Test suites

| Suite | Scope |
|---|---|
| `test_core` | SHA-256 against FIPS 180-4 vectors, canonical codec, reason codes, checked arithmetic, identity types, capability parsing, full durable-state round trip |
| `test_placement` | Eligibility, policy constraints, capacity and reserve, batch conservation, ranking and tie-breaking, sticky retention, fallback, locality, idempotency, malformed input, evidence ordering and conflicts, stale generations, topology invalidation, bounded rebalance, explanations, canonical export |
| `test_migration` | Contract requirements, authority, acknowledgement before effect, effect before verification, exclusive state, concurrent attempts, target disappearance, incarnation changes, revocation, stateless moves, fencing |
| `test_persistence` | Round trip, clean reopen, torn tail, mid-file corruption, corrupt/truncated snapshot, incompatible version, oversized record, compaction, restart epoch and fencing, liveness not resurrected, effect demotion, watermarks, unusable store |
| `test_property` | Seeded randomized scenarios: order independence, conservation, deterministic fallback, stale-generation avoidance, exclusive state, restart invariants, canonical decoding of random bytes, every truncation of a valid encoding, bounded deterministic rebalance, watermark monotonicity |
| `test_concurrency` | Serial versus concurrent ingest equivalence, concurrent readers during writes, asynchronous submit/wait/cancel, repeated open/shutdown, shutdown with work in flight, determinism across concurrent submissions |
| `test_transport` | Frame codec against every malformed shape, full control surface over a real loopback socket, adversarial frames, concurrent clients, shutdown frame |
| `test_process` | Independent OS process serving over a real socket with a differential comparison against an in-process engine, hard kill and restart, CLI recovery across processes, interrupted commits at six durable boundaries, damaged store refused by a separate process |

No test uses a timeout, a sleep, or a poll loop. Synchronisation is always on
completed work: a join, a condition-variable wakeup, a line read to end of
stream, an exit status, or a protocol response.

## Sanitizers

AddressSanitizer is **not available** in the toolchain used here: the MSVC
installation has `asan_compat.lib` but not the x64
`clang_rt.asan_dynamic-x86_64` runtime, so `/fsanitize=address` cannot link.
No sanitizer result is claimed. The compensating measures are listed in
`README.md` section 12.

## Adversarial passes

Performed after the first fully green run, with the suite rerun afterwards:

* Integer extremes: `UINT64_MAX` demands, capacities, sequences and timestamps.
* Absurd sizes: oversized placement batches, oversized journal record lengths,
  oversized snapshot payloads, oversized frames, over-long strings.
* Duplicate identities: repeated requests, repeated evidence, repeated
  acknowledgements, conflicting duplicates at every level.
* Stale authority: expired policy, schedule, topology, incarnation, capability
  and boot generations presented to each entry point in turn.
* Reordered and duplicated evidence: replayed sequence numbers, out-of-order
  observations, two sources disagreeing at the same instant.
* Partial writes and journal damage: torn tails, mid-file corruption, header
  damage, version damage.
* Cancellation at awkward boundaries: cancellation before start, during run,
  after completion; shutdown with a full queue.
* Target disappearance mid-migration, incarnation replacement mid-attempt.
* Real process death: hard kill of the service process and of the CLI at six
  durable boundaries.
* Lock audit: read-then-write reacquisition, callbacks under locks, nested
  locks, worker joins while holding worker-needed state, cancellation lock
  inversion, shutdown ordering, event emission under locks, acquisition order.
