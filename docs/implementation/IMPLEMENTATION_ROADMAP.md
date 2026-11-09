# GitTrace implementation roadmap

**Created:** 2026-11-08  
**Basis:** [`docs/product/PRODUCT_REVIEW.md`](../product/PRODUCT_REVIEW.md), [`docs/product/PRODUCT_PLAN.md`](../product/PRODUCT_PLAN.md), current implementation, and captured A–D retrieval results  
**Roadmap goal:** Make GitTrace trustworthy about source history, faster on repeat runs, and measurable on realistic developer queries.

## Implementation status — 2026-11-08

- **M0 started:** JSON profile output records repository initialization, model load, history walk, grammar preparation, parse, chunk/embed, SQLite load/save, counts, cache-hit state, and per-query elapsed time. Fine-grained retrieval/reranker substage timings remain open.
- **M1 substantially implemented:** file versions retain commit/tree/path/blob occurrences; fixtures cover unchanged states, rename, identical content at multiple paths, a side branch, a merge, and ignored files. The selected history scope remains commits reachable from `HEAD`.
- **M2/M3 core implemented:** SQLite stores chunks, `float32` embeddings, and occurrence metadata. Cache fingerprints include nested/global ignore inputs, grammar revisions and parser/scanner file metadata, Tree-sitter ABI/compiler, model file metadata, platform, chunker, and embedding-input versions. `--index`, `--index-status`, and `--rebuild-index` are available. A SQLite transaction lock serializes indexers and saves replace snapshots atomically.
- **Incremental descendant refresh validated:** a new commit walked only the one new commit, embedded only one new chunk (instead of two in a clean rebuild), and produced byte-for-byte identical chunk and occurrence rows after stable sorting. An allow-empty metadata commit added the new occurrence with zero embedding calls and zero model-load time. Divergent histories rebuild the occurrence map using cached embeddings; full rebuild is the fallback when the cache configuration changes.
- The cache uses model path/size/mtime rather than a full model content hash. Fine-grained changeset traversal is commit-incremental, but each new commit tree is still walked. Run-level benchmark coverage remains one small fixture; incremental indexing on a representative repository has not been measured.
- Build and both registered CTest tests pass after the current code changes. A full-repository CPU cold run was stopped because the complete corpus embedding was taking a long time; the cache smoke test used a one-chunk repository instead.

## Direction

Implement in this order:

1. Establish correctness checks and performance baselines.
2. Preserve complete Git provenance and define the history scope.
3. Persist a versioned local index and reuse embeddings.
4. Make refresh incremental and safe to interrupt.
5. Improve retrieval against a held-out benchmark.
6. Polish installation, privacy, and result presentation.

SQLite is the first persistence choice, with embeddings stored as `float32` blobs. Do not build a daemon, custom binary format, or ANN index until measurements show the persisted single-process design needs them.

## Current starting point

Already in place: vector, keyword, and hybrid retrieval; optional query expansion and reranking; separated raw cosine, lineage-adjusted similarity, and retrieval score; one-shot repeatable queries; tests for duplicate content at separate paths, renames, ignore rules, CLI options, and lexical fixtures; A–D full-repository output captures.

Known gaps: only four hand-selected retrieval queries; no complete commit/tree/path occurrence map; heuristic lineage; no persistent index; repeated history scanning/parsing/embedding; no stage-level benchmark; limited result diagnostics and configuration portability.

## Milestones

### M0 — Instrument and freeze the baseline

**Purpose:** Know what costs time and preserve a reproducible reference before changing the pipeline.

**Implementation tasks**

- Add structured stage timings and counts for repository/ref discovery, commit/tree walk, blob reads, grammar preparation, parsing, chunking, corpus embedding, index save/load, query embedding, lexical/vector ranking, and reranking.
- Emit a compact machine-readable run summary (JSON or JSONL) with GitTrace version, repository identity, selected refs, index configuration, chunk count, embedding count, result ranks, and elapsed time.
- Save one cold-run baseline and repeat it on an unchanged second invocation for a small repository and a representative larger repository.
- Record stable ranking output for the existing A–D queries with expansion/reranking configuration explicit.

**Acceptance criteria**

- Every expensive startup stage has a separate duration and count.
- Two runs with the same source and configuration can be compared without manually copying terminal text.
- No ranking behavior changes in this milestone.

**Dependencies:** None. Do this before performance work.

### M1 — Make Git provenance exact

**Purpose:** Ensure every returned chunk can be tied to the right source path and Git state.

**Implementation tasks**

- Define initial scope as commits reachable from `HEAD`; expose the selected ref/commit in index metadata and CLI output. Do not imply all branches are indexed.
- Split reusable chunk content from its Git occurrences. Represent `(commit SHA, tree/path, blob OID)` membership explicitly, including unchanged states across commits.
- Preserve multiple paths for identical content and every commit occurrence needed to support the chosen scope. Keep source text/embedding reusable when the same parsed chunk recurs.
- Add fixtures for unchanged content across commits, rename, identical content at two paths, ignored paths, a side branch, and a merge. Assert expected path, commit, line range, and source text.
- Replace implicit lineage-depth authority with explicit inferred relationship metadata (method, confidence, and related chunk/commit). Keep lineage adjustment independently switchable while validating the matcher.

**Acceptance criteria**

- Every fixture result reports a valid occurrence in the selected history scope.
- A chunk cannot inherit a commit/path association from a different occurrence.
- Branches outside the declared scope are either excluded clearly or selected explicitly; no silent scope ambiguity.
- Existing tests continue to pass.

**Dependencies:** M0 test/run reporting. This schema decision precedes persistent storage.

### M2 — Add a versioned SQLite index

**Purpose:** Avoid rebuilding the same index and re-embedding unchanged code every time GitTrace starts.

**Implementation tasks**

- Add an index manager with a repository-local default (for example `.gittrace/index.sqlite`) and a CLI/config override.
- Create versioned tables for index manifest, Git commits/trees/occurrences, reusable chunks, embeddings, and optional lineage/lexical derived data.
- Store embeddings as explicit contiguous `float32` BLOBs with dimension and byte-length checks. Never serialize native C++ structs or pointers.
- Store a manifest fingerprint covering schema, model identity/fingerprint, vector dimension/pooling, corpus encoding, chunker version/config, relevant grammar revisions, ignore rules, and selected ref scope.
- Implement load, validate, full rebuild, and `--rebuild-index` behavior. If fingerprints mismatch, explain which layer is stale and rebuild safely.
- Use transactions, prepared statements, a per-index lock, and staging/atomic replacement so interruption cannot leave a partially valid index.
- Keep query-only startup independent from corpus model initialization when a valid vector index already exists; keyword-only operation should not load the embedding model.

**Acceptance criteria**

- A clean build and a loaded index return identical results and provenance for the same configuration.
- Reload validates embedding dimensions, schema, repository identity, and fingerprints.
- Simulated incomplete/corrupt index state fails closed with an actionable message and can be rebuilt.
- No corpus embedding occurs on an unchanged warm start.

**Dependencies:** M1 occurrence/data model; M0 instrumentation.

### M3 — Incremental refresh and resumability

**Purpose:** Make updates scale with new history/content rather than the entire repository.

**Implementation tasks**

- Track indexed commits and refs. During refresh, find newly reachable commits for the selected scope and add only missing occurrence rows.
- Parse/chunk/embed only unseen eligible `(blob OID, path, parser/chunker configuration)` states. Reuse vectors where the content and embedding input are identical.
- Cache parsed spans and compiled Tree-sitter grammars against grammar/source revision and build environment fingerprints.
- Separate invalidation layers: occurrence scope, ignore visibility, grammar/parser, chunker, embedding model/encoding, lexical statistics, lineage. Begin with safe full rebuilds for unknown changes; add narrow invalidation when each rule is testable.
- Add `--refresh-index`, `--rebuild-index`, and a status command/report showing indexed refs, commit, counts, and stale reason.
- Ensure an interrupted refresh can resume or safely roll back; test concurrent process lock behavior.

**Acceptance criteria**

- Adding a commit with one changed file embeds only newly required chunks.
- A new commit that reuses existing blobs adds occurrence metadata without re-embedding unchanged chunks.
- An unchanged second run makes zero corpus embedding calls and produces stable results.
- Model/chunker/grammar/ignore/ref changes cause the expected invalidation and no silent stale reuse.

**Dependencies:** M2 stable schema and manifest.

### M4 — Build a credible retrieval benchmark

**Purpose:** Replace tuning against four known queries with a generalization check.

**Implementation tasks**

- Create at least 30 developer-authored queries across multiple repositories and languages. Include behavioral descriptions, identifiers, misspellings, path hints, current-code questions, history questions, and unanswerable cases.
- Label direct evidence spans and relevant commit IDs. Separate “direct answer evidence” from context that is merely related.
- Split queries into tuning and held-out sets before changing synonym maps, rank fusion, or thresholds. Keep A–D as visible diagnostic cases.
- Run vector, keyword, hybrid, expanded, and reranked conditions on the same index/query/top-*k* budget. Add `git grep` for suitable working-tree keyword tasks, clearly noting the history-scope difference.
- Report per-query outcomes plus MRR@10, Recall@5/10, Precision@5, target-commit accuracy, answerable/unanswerable behavior, and latency.
- Add diagnostics for vector rank, lexical rank, matched terms, expansion terms/weights, lineage contribution, and final score.

**Acceptance criteria**

- Labels and test split are versioned and reproducible.
- Every aggregate metric can be traced back to individual queries and evidence spans.
- C-like misses remain visible rather than being obscured by an average.
- Do not enable a no-result threshold or promote expansion based only on training results.

**Dependencies:** M0 machine-readable capture. M1 improves label/provenance accuracy; benchmark construction can begin in parallel with M1/M2.

### M5 — Improve ranking and chunk context

**Purpose:** Raise recall and precision through evidence-backed changes rather than query-specific patches.

**Implementation tasks**

- Add bounded contextual chunk representations with path, enclosing symbol, syntax kind, signature, and useful adjacent comments/configuration; keep display text and source line provenance exact.
- Search code, path, symbol, and metadata fields separately. Tune BM25 weights and reciprocal-rank fusion on the training split.
- Evaluate exact identifier/path matching as a high-precision lane; keep normalization/expansion lower-weight and independently measurable.
- Compare lineage-aware ranking to a no-lineage baseline. Promote lineage only if it improves history queries without regressing current-code queries.
- Measure candidate recall before reranking; test candidate over-fetch against latency. Keep reranker failure fallback explicit and mode-correct.
- Calibrate a “no strong match” behavior on the held-out set; otherwise show ranked candidates without implying confidence.

**Acceptance criteria**

- Improvements hold on held-out queries and do not come solely from A–D aliases.
- Every displayed source span supports its path/commit metadata.
- Retrieval and reranking effects can be isolated in the evaluation report.
- Any no-result threshold meets a predeclared false-positive/false-negative target.

**Dependencies:** M4 labeled benchmark; M1 exact occurrence model.

### M6 — Portability and release readiness

**Purpose:** Let a new user build and run GitTrace without developer-specific paths or hidden network behavior.

**Implementation tasks**

- Remove machine-specific CMake dependency defaults. Add explicit config/CLI/environment paths for model, grammar cache, and index.
- Resolve paths from executable/config/repository context rather than requiring the current working directory to be the source checkout.
- Pin grammar revisions and reuse compiled artifacts. Document when network/compiler access is needed and how to run offline with existing grammars/model.
- Make local retrieval independent of Laya/Gemini. Clearly disclose query/source transfer and possible API cost before selecting a remote reranker.
- Add clean macOS/Linux build instructions and verify a fresh build, fresh index, warm index, refresh, and recovery workflow.
- Document storage location, cache deletion, index rebuild, history scope, ignore semantics, and score interpretation.

**Acceptance criteria**

- A new checkout can be configured without editing source paths.
- Local keyword/vector/hybrid search works without a reranker service.
- First-run network needs and all external data flows are clear.
- Build and end-to-end workflows pass on supported platforms.

**Dependencies:** M2/M3 index lifecycle; M5 stable result behavior.

## Sequencing and parallel work

```text
M0 Baseline/instrumentation
  └── M1 Exact provenance ── M2 SQLite persistence ── M3 Incremental refresh
          │                       │                         │
          └────────────── M4 Benchmark ── M5 Retrieval quality
                                                           └── M6 Release readiness
```

Benchmark query collection (M4) can start while M1/M2 are being implemented. Schema and cache invalidation decisions should not be finalized before M1 defines occurrence identity and M0 records the current work. Ranking tuning should wait for the held-out split.

## Scope controls

- **Do now:** exact provenance, baseline timings, SQLite persistence, no-reembed warm starts, labeled evaluation.
- **Do after measurements:** batching, parser parallelism, memory-mapped embedding matrix, ANN, background service.
- **Do not add as a shortcut:** hand-authored A–D synonym growth, uncalibrated confidence cutoffs, or treating reranker judgments as verification.

## First implementation slice

Start with M0 and the minimal M1 fixture/schema design, then implement M2 as a read/write cache for the current deterministic index before attempting fine-grained incremental updates. The first end-to-end demo should show:

1. First run scans and builds the index, logging per-stage timings.
2. Second unchanged run loads the index without corpus embeddings.
3. A new commit refreshes occurrence metadata and only embeds new chunks.
4. Changing the embedding model or chunker triggers a clear invalidation/rebuild.
5. Search results and full provenance match a clean rebuild.

This sequence proves the proposed speedup and the cache's correctness before optimizing the database format or adding a service process.
