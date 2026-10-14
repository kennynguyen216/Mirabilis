# Baseline player collision fixtures

The 23 JSON files in `ecs_collision/` were dumped from the baseline engine at
`5486a56` plus instrumentation commits `c7863a7` and `5da889d`. They are measured
engine output, not collision geometry reconstructed by the tests.

The Release executable SHA256 was
`e314a06220f39577e4481ad894392e661caf4e8649f742b8b9d3796bb83e60c1`.
`ecs_collision_manifest.json` records each source scene hash, fixture hash and
ordered array counts. Authored portal pairs were loaded, player portals remained
unplaced, and the dump ran before the bounded harness modified portals.

Compare array lengths and order exactly. Every number uses absolute tolerance
`1e-5` plus relative tolerance `1e-6 * abs(baseline value)`. Dumps use the float
`max_digits10` precision (9 decimal digits). Ramp matrices are column-major.
Ground `center` and `halfExtents` contain X/Z coordinates, in that order.

The extraction checkpoint reproduced all 23 fixtures numerically exactly.
Scene round trips compare authored entities/components/settings, excluding runtime
IDs and allowing the persistent allocator to retain its high-water mark.
