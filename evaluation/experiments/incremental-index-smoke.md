# Incremental index smoke test

**Date:** 2026-11-08  
**Platform:** macOS arm64

Two temporary repositories exercised descendant refresh.

## New file state

Started with one C++ file/chunk, indexed it, added a second C++ file in one new commit, then refreshed. GitTrace visited one new commit, retained the old chunk, and embedded only the new file state: one embedding call versus two in a clean rebuild. The final SQLite chunk rows and occurrence rows were compared with a forced clean rebuild and matched exactly after stable ordering.

## Metadata-only commit

Added an allow-empty commit with no tree changes. Refresh visited only that commit, added a second occurrence for the existing chunk, and made zero embedding calls; it also skipped embedding-model initialization.

The final repeated run also measured manifest fingerprinting at about 5.4 ms on this small fixture. Machine-readable counters are in [`incremental-index-smoke.json`](../incremental-index-smoke.json). These are correctness smoke fixtures, not representative large-repository timing results. The refresh still walks each new commit's full tree, and no large-repository incremental benchmark has been run.
