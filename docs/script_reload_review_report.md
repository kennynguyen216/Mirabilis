# Independent review of live script reload

The user requested a fresh independent review and authorized merging and pushing
to main only after it passed. This report supplements the original implementation
report, `docs/script_reload_report.md`.

## Candidate and reviewer

- Original candidate: `fa18bba3a5eeae345d96f44ecc4b245fa621e5e3`.
- Corrected implementation: `dad341b124b9706f9a37adfbd14aa50d11ea811b`.
- Fresh independent reviewer: GPT-6 Astra, High reasoning,
  `/root/hot_reload_independent_review`. The reviewer changed no files.
- Lead: Codex; exact runtime model label is not exposed by the tools. The lead
  implemented the corrections and regression tests. At most two agents were active.
- Expected use: one Windows editor process compiling local script projects.
- Evidence: `C:/dev/mirabilis-work/reload-review-20261017`.

Main and freshly fetched origin/main both started at reviewed physics tip
`e46908d3e10cd17276a2410c6b597a8ae05123cb`. The feature is its descendant and
includes the previously independently approved scripting milestone (`824fdab`,
report `3d33fc3`) plus hot reload. The original main checkout was clean.

## Initial findings and corrections

The original review verdict was **FAIL**, with two P2 blockers. No additional
merge blockers were found after reviewing transaction/rollback, native mutation
suppression, lifecycle, supported/private/inherited/nonserialized fields,
collectible contexts/dependencies, compiler jobs and staging, watcher controls,
frame integration, ABI rejection, and connected tests.

1. Public-field binding assigned runtime values by public name before strict
   migration. Moving a public field from a base class to the concrete script
   incorrectly kept 42 instead of its new initializer 99. Reload now validates
   saved data without assigning it; runtime assignment uses only the declaring
   type, name, and field type match.
2. Rebinding a previously missing saved entity ID during reload could then copy
   the old default entity over it and serialize null, losing the saved ID.
   Reload now carries prior unbound-field bookkeeping forward. Normal fresh
   creation still binds authored fields, including a previously missing target
   that has appeared.

The lead added two-version fixtures and regressions before changing the runtime.
Both cases failed on the original code: 2 cases, 293 assertions, 4 failures.
`regressions-before-fix.log` retains that proof. The fix reuses the existing saved
field validator with an optional assignment flag and preserves the existing
Unbound set. Normal creation uses the unchanged default assignment behavior.
Contracts and author guidance document the distinction.

The reviewer re-read corrected commit `dad341b`, then independently verified the
completed corrected-run evidence. **Final gate verdict: PASS — ship `dad341b`.**
No outstanding blockers or validation conditions remain.

## Validation

| Check | Final result | Evidence relative to the review evidence directory |
| --- | --- | --- |
| Corrected Debug build | PASS | `build-debug-corrected.log` |
| Corrected Release build | PASS | `build-release-corrected.log` |
| Debug CTest | PASS, 7/7 suites; 79 native cases / 175,268 assertions; 105 managed assertions | `ctest-debug-corrected.log` (41.69 s) |
| Release CTest | PASS, same suites/counts | `ctest-release-corrected.log` (35.65 s) |
| Real source save, compile and in-Play reload, Debug | PASS; world and private state retained | `gpu-debug-corrected/` |
| Real source save, compile and in-Play reload, Release | PASS; world and private state retained | `gpu-release-corrected/` |
| Original bugs reproduced before correction | Both new cases fail, 4 failed assertions | `regressions-before-fix.log` |
| Corrected provenance and authored-data preservation | PASS; 146 source hashes, 6 binary hashes, all 4 protected hashes; both GPU records preserve all 26 authored files | `corrected-verification.json`, GPU `preservation.json` |
| Independent final evidence review | PASS; no remaining conditions | `final-review.md` |

Debug's live reload continued at frame 233 (Updates 233, private observation 273),
and Release at frame 4695 (Updates 4695, private observation 4735). Both retained
epoch 3, 3 bodies, 1 instance, velocity 0.25 and Creates=1. The next frame used
the new +10 update behavior while private state advanced by one.

The original implementation's evidence remains in
`C:/dev/mirabilis-work/reload-check`, including the Python, portal, scene, input,
lifecycle and baseline comparisons. This correction changes managed reload/binding
and test fixtures; it adds no dependencies and changes no rendering settings.

## Authored data and publication

The user's existing feature-worktree changes were not staged or committed:

| File | SHA-256 |
| --- | --- |
| Feature `assets/scenes/.last_scene` | `f922093298c0ddd64b8bf545164113332fa6f7bbaf27ef8ee86d2209e440c517` |
| Feature `assets/scenes/scripted_physics_demo.json` | `c6ef9a48431c44e8a197267d8df35fc8082415874fdd6be225662b494936dab9` |
| Physics worktree `.last_scene` | `cc8088a7cd1690466ee1b1bc6154f25bc6ce5e9fc0bbab6bae796ee67f0f9067` |
| Original main checkout `.last_scene` | `3ccdc36fac8edae9eb9795f2f4bf3c08f650c5b1421cc9e3c893b1f2981f6c96` |

Builds and evidence remain outside OneDrive. Save/load tests use scratch scenes.
The user authorized a fast-forward merge to main and normal push after the final
PASS. The final Git publication and preservation checks are recorded in
`publication.json` in the evidence directory, with the published revision also
reported in the final response. No forced push is authorized or required.

## Limits and reproduction

The supported six field types and other limitations are unchanged; see
`docs/scripting.md`. Complex object graphs, collections, async state machines,
and externally retained script threads/delegates are not migrated. Core/native
changes require restart. No new performance sweep, complete renderer/cache sweep,
or manual inspector interaction sweep was performed for this review.

Use the existing outside-OneDrive build:

```powershell
cmake --build C:/dev/mirabilis-work/build-scripting --config Debug --parallel 4
ctest --test-dir C:/dev/mirabilis-work/build-scripting -C Debug --output-on-failure
```

For real source-save validation, choose a fresh evidence directory and run
`tests/script_reload_gate.py` with `--repo`, `--output`, and `--configuration`
as described in the original report. Launch the built editor from
`C:/dev/mirabilis-work/scripting/bin/Debug`; save a user C# source file during Play.

## Additional local commits

- `dad341b` fixes both independently identified migration defects and adds their
  regressions and author/contract clarification.
- Documentation-only review report commit follows the final gate approval.
