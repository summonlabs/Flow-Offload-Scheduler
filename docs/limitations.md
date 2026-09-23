# Limitations

Genuine limitations, stated so that a reader does not have to discover them.

1. **Journal write amplification.** Each commit record carries the whole durable
   state, so per-commit cost grows with state size. Compaction bounds the
   journal's total size, not the per-commit cost. A delta journal is the
   natural next step.
2. **Commit under the engine lock.** Durable commits happen with the state lock
   held. This makes ordering trivially correct but serialises mutating calls
   behind the device. Read-only queries are unaffected.
3. **Evidence is not durable.** After a restart, load evidence must be
   re-supplied before anything can be placed. This is deliberate: a persisted
   observation could look current when it is not.
4. **Topology advancement is blunt.** A newer topology generation marks every
   target declared under an older one as not live, so all of them must be
   re-declared.
5. **Rebalance improvement estimates ignore the source release.** Each flow's
   improvement is computed against the pre-move state, so a large batch can be
   slightly optimistic about a destination's projected utilisation. The
   capacity check at apply time is exact, so this affects ordering preference,
   never safety.
6. **Bounds are compile-time constants**, not runtime-configurable per engine.
7. **One store directory per engine.** Two coordinators on the same directory
   fence each other's authority rather than cooperating; there is no
   multi-writer coordination.
8. **Recommendation and authorization history is in-memory and bounded.**
   Exceeding the bound evicts the oldest entry and counts the eviction. A caller
   holding an evicted identifier receives `RejectedRecommendationMismatch`
   rather than a stale grant.
9. **Sockets are IPv4 loopback-oriented.** The service binds an IPv4 address;
   IPv6 and TLS are not implemented.
10. **The transport is not authenticated.** It assumes a trusted, local
    neighbour, consistent with the boundary: this runtime has no security
    product surface.
11. **Only MSVC on Windows was built and tested.** No GCC, Clang, or POSIX
    result is claimed.
12. **No sanitizer coverage.** See `docs/validation.md`.
13. **No hardware validation of any kind.** Every target in the test suite is a
    synthetic descriptor.
