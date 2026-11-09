# GitTrace product and engineering review

**Reviewed:** 2026-11-08  
**Scope:** Current C++ CLI, historical indexing, retrieval, reranking, and repeat-run performance  
**Evidence:** Repository source and the A–D retrieval captures under [`evaluation/`](../../evaluation)

## Summary

GitTrace has a useful prototype foundation: it searches code across Git history, supports lexical/vector/hybrid retrieval, shows source locations and commit IDs, and can optionally rerank a candidate list. The most important weaknesses are trustworthiness and repeated startup cost. Search still misses a known safety-related target in the four-query evaluation; history metadata is not a complete commit-to-path occurrence map; and each invocation walks, parses, chunks, embeds, and indexes history again.

The next work should make evidence/provenance reliable, persist a versioned local index, and build a broader relevance benchmark. A custom binary snapshot could work, but SQLite is the recommended first persistence layer: it supports atomic updates, schema migrations, metadata queries, and recovery without inventing a file format. Keep embeddings as binary float data in SQLite blobs (or a sidecar file only if profiling proves SQLite I/O is the bottleneck).

## Implementation update — 2026-11-08

The implementation now records stage timings, preserves commit/tree/path/blob occurrences, and saves chunks plus embeddings to a local SQLite cache. Compatible warm runs skip corpus indexing; descendant commits update occurrence metadata and embed only unseen file states. Writer locking and atomic cache replacement are in place. See [`docs/implementation/IMPLEMENTATION_ROADMAP.md`](../implementation/IMPLEMENTATION_ROADMAP.md) and [`evaluation/`](../../evaluation) for smoke/incremental results. Representative-repository performance and model content hashing remain open.

## Current weaknesses and recommended fixes

| Weakness | Evidence and impact | Recommended fix | Priority |
|---|---|---|---|
| Search quality is not dependable yet | In the documented A–D top-three runs, the Gemini candidate-safety target is absent in vector, keyword, hybrid, and expansion conditions. Expansion helps A/B but expanded hybrid drops D. A top-*k* list is not proof that any result is useful. | Build a larger labeled query set across repositories and languages. Record direct evidence spans and answerability. Add a calibrated no-strong-match state only after measuring thresholds. Keep expansion opt-in until it does not regress held-out cases. | P0 |
| History provenance is incomplete | The walker starts at `HEAD`; chunks represent unique `(blob ID, path)` states, but the indexed record retains only one commit SHA. A path's unchanged state across many commits, branches, or merges cannot be reconstructed exactly from the chunk alone. | Model content/chunks separately from occurrences. Store every relevant `(commit, tree, path, blob)` occurrence or a lossless compressed range/ref representation. Add branch, merge, rename, and unchanged-file fixtures. | P0 |
| Lineage is heuristic | `inferLineages()` pairs same-path, same-syntax chunks using embedding similarity and ordinal proximity; it does not follow Git parent edges or detect moves. Decay can demote a valid historical match based on an uncertain relationship. | Treat lineage as inferred metadata with confidence and evidence. Compare against explicit parent-tree diffs and rename detection. Make current-vs-history preference a query/ranking policy, not an invisible proxy for correctness. | P0 |
| Chunks can lack retrieval context | Embeddings are generated from source span text. Tiny declarations may omit path, symbol, syntax kind, and nearby configuration context. A relevant concept may never appear in the embedded text. | Build bounded, AST-aware chunk context: path, enclosing symbol, syntax kind, and adjacent signature/docstring/configuration as separate fields. Preserve the exact source span separately for display and citation. | P1 |
| Retrieval ranking diagnostics are limited | Hybrid uses reciprocal-rank fusion with one shared BM25 text field. The CLI exposes final scores, but users cannot inspect lexical/vector ranks, matched terms, expansion, or lineage contribution. Similarity scores are not calibrated relevance probabilities. | Add a diagnostic output mode with per-lane ranks, matching terms, expansion weights, raw cosine, lineage adjustment, and fused score. Tune field weights and fusion on training queries, then report held-out results. | P1 |
| Reranking is downstream and cannot restore recall | Laya/Gemini sees only the candidates supplied by retrieval. The service can be slow or unavailable; Gemini sends query and source excerpts to an external API. | Measure candidate recall before reranking, over-fetch within a latency budget, make remote disclosure explicit, and retain a clearly labeled local fallback. Do not call reranking factual verification. | P1 |
| Startup repeats expensive work | The index is in memory and rebuilt on every run. `beginGitTrace()` walks commits and trees, collects blobs, prepares grammars, parses files, chunks spans, and embeds each chunk. | Persist the index and add incremental updates keyed by repository and index configuration. Reuse unchanged content and embeddings. Measure each stage before adding concurrency or ANN indexing. | P0 |
| Grammar setup can add first-run and repeat-run cost | Grammar repositories are cached locally, but README/source indicate grammar parser libraries are rebuilt during preparation; first use can clone and compile. | Cache compiled grammars by grammar revision, compiler/platform/ABI, and build options. Skip work when a valid artifact exists; provide clear offline/prebuilt behavior. | P1 |
| Configuration is fragile | The embedding model and grammar locations are relative to the current working directory, and CMake has a developer-specific libgit2 prefix default. | Add explicit CLI/config/environment paths, discover repository root robustly, and make clean-machine build/install instructions reproducible. | P1 |

## Persistence design: save the index, not just a dump of current memory

Saving embeddings after the first run is the right direction. The cache must include enough metadata to decide whether it is safe to reuse them. Reusing vectors after changing the model, chunking, ignore rules, or query/corpus encoding would silently produce misleading results.

### Recommended first implementation: SQLite

Use a repository-local database such as `.gittrace/index.sqlite` (or a user cache keyed by canonical repository path). Store a schema/version manifest and separate logical records:

- **Repository/index manifest:** canonical repository identity, schema version, GitTrace version, embedding model identifier and file fingerprint, embedding dimensions, query/corpus encoding version, chunking version, grammar revisions, ignore-rule fingerprint, and last indexed refs/commit IDs.
- **Content/chunk records:** blob OID, relative path or path-independent content identity, chunk source text, line range, syntax metadata, chunking version, and embedding bytes (`float32` initially).
- **Occurrence records:** commit SHA, tree/path, blob OID, author/time if needed, and the chunk IDs present at that occurrence. This preserves provenance while allowing content or embeddings to be reused.
- **Optional derived data:** tokenized lexical terms/document statistics and lineage edges with algorithm version/confidence. These can be rebuilt if their version changes.

Use prepared statements and transactions. Build updates in a transaction or staging tables, validate row counts and manifest, then commit atomically. Keep the prior valid index until the new one is complete. On corruption or incompatible schema, report the reason and rebuild rather than partially loading. Add a lock so two GitTrace processes cannot update the same index at once.

### Cache invalidation rules

Reuse stored chunks/embeddings only when the relevant inputs match:

1. **Embedding identity:** model bytes or stable model revision, vector dimension, pooling/normalization behavior, and corpus encoding template.
2. **Chunk identity:** blob OID, path where path affects parsing, parser/grammar revision, and chunker version/configuration.
3. **Visibility:** effective ignore rules and selected Git refs/commit scope.
4. **Derived scores:** lineage and lexical indexes include their own algorithm/version marker and can be rebuilt independently.

For incremental history updates, scan newly reachable commits and trees, record new occurrences, and process only unseen eligible file states. A changed ignore policy or chunking/model version should invalidate the affected layer, not necessarily every other layer. A simpler first release can invalidate and rebuild the whole index when its manifest changes; correctness comes before fine-grained invalidation.

### Why not start with a custom binary file?

A binary snapshot can load quickly and compactly, but it creates responsibilities for format versioning, partial-write recovery, migrations, locking, random access, and corruption handling. A naïve `ofstream` dump of C++ structs is unsafe because strings/vectors contain process-specific pointers and layout may change between builds. If a binary format is selected later, define explicit endianness, lengths, schema version, checksums, and atomic temp-file/rename writes. Benchmark it against SQLite before taking on that maintenance burden.

SQLite can store each embedding as a contiguous `float32` BLOB, avoiding text serialization overhead. If profiling later shows database reads dominate, mmap a dedicated append-only embedding matrix while retaining SQLite for metadata and transactions.

## Ways to make GitTrace faster

### Biggest expected win: avoid repeating embeddings

The current startup performs model loading and corpus embedding before queries. Persisted vectors remove the most expensive repeated model work for unchanged content. Separate the CLI flow into:

1. Open and validate the repository index.
2. If absent/stale, update it and report stage timings.
3. Load/query the ready index without embedding unchanged chunks.

Keep the embedding model unloaded in keyword-only mode and allow explicit `--rebuild-index` / `--refresh-index` controls. If embeddings are disabled, do not parse/chunk/embed solely to serve keyword retrieval unless that index is independently stale.

### Make history scanning incremental

Walking every commit and every tree costs work even if embeddings are cached. Record indexed refs and known commits; on refresh, traverse only newly reachable commits and add occurrence rows. Reuse Git object IDs to avoid reading identical blobs more than once. Be explicit about whether the product indexes `HEAD`, all local refs, or selected refs; a cache is only correct for the declared scope.

### Reuse grammar builds and parse outputs

Cache compiled Tree-sitter grammar libraries with a manifest of source revision and build environment. Cache parsed spans by `(blob OID, path/extension, grammar version)` so an unchanged file state does not need parsing again. Avoid network access when cached grammars suffice.

### Profile before optimizing query ranking

At 224 chunks, full linear vector scoring and sorting are not the likely bottleneck. Keep exact scans until measurements on larger corpora say otherwise. Add per-stage timings and counts for Git walk, blob read, grammar preparation, parse, chunk, embed, persistence load/save, query embedding, lexical ranking, vector ranking, and reranking. Use those measurements to choose whether batching embeddings, parallel parsing, ANN, or a different storage layout is worthwhile.

### Low-risk implementation improvements

- Batch embeddings if llama.cpp supports it for this model and verify that batched output matches current single-input vectors before adopting it.
- Precompute/store normalized vectors if cosine profiling shows repeated norm work matters; preserve the raw vector if score interpretation needs it.
- Avoid repeatedly scanning a buffer from its beginning for line numbers in fallback chunking; maintain line offsets in one pass.
- Reuse tokenized chunk terms and corpus statistics for BM25 rather than tokenizing the entire corpus for each query.
- Keep stable tie-breaking so cached and uncached runs produce the same rankings.
- Do not parallelize model calls or Git/Tree-sitter access until thread-safety and resource usage are measured.

## Recommended implementation sequence

### P0 — Correctness and baseline

1. Preserve exact commit/tree/path occurrences and add fixtures for unchanged content across commits, rename, branch, merge, and ignore behavior.
2. Add timing instrumentation and record cold-run stage timings on a small and a larger repository.
3. Grow the labeled query set beyond A–D; include unanswerable queries and a `git grep` baseline for working-tree keyword searches.
4. Keep query expansion and reranking optional while measuring retrieval-only candidate recall.

### P1 — Persistent local index

1. Implement a versioned SQLite schema for manifest, chunks, occurrences, embeddings, and optional derived terms.
2. Add safe load, atomic rebuild/update, lock, corruption recovery, and explicit refresh/rebuild controls.
3. Add a warm-start test asserting unchanged runs reuse stored vectors and return the same ranked results as a clean rebuild.
4. Measure database size, cold indexing time, warm load time, warm query latency, and invalidation behavior.

### P2 — Incremental indexing and retrieval quality

1. Update only newly reachable commits and unseen file states; preserve all path/commit occurrences.
2. Cache parser outputs and grammar artifacts by stable revision.
3. Add path/symbol/syntax context and separate retrieval fields.
4. Use labeled training queries to tune BM25, vector/hybrid fusion, and lineage treatment; evaluate once on held-out queries.

### P3 — Product polish

1. Add a useful “no strong match” behavior backed by labeled calibration.
2. Show current/historical status, exact provenance, and concise match evidence.
3. Make model, grammar, cache, and repository paths configurable; remove machine-specific build defaults.
4. Document data flow and external reranking consent; provide a clean install and offline workflow.

## Product recommendations

- Treat GitTrace as a **local code-history index with evidence-backed retrieval**, not an LLM answer engine.
- Make correctness and source provenance the release gate; a fast, wrong commit citation is worse than a slow index.
- Implement SQLite persistence before a background daemon. A daemon may help later with parallel indexing and interactive clients, but persistence alone addresses the repeated embedding cost with less operational complexity.
- Keep vector, keyword, hybrid, and reranker stages inspectable and independently switchable.
- Keep Gemini opt-in and visibly disclose that query/source candidates leave the machine. Keep local retrieval useful without any service running.
- Do not add ANN/vector database infrastructure for the current corpus. First measure the persisted, incremental index on representative larger repositories.

## Success measures for the next milestone

- A second unchanged run performs zero corpus embedding calls and returns the same results as a clean build.
- Index updates preserve exact commit/path provenance across tested branches, merges, renames, and unchanged files.
- Cold indexing, warm load, and query latency are separately measured and published for at least two repository sizes.
- Retrieval is evaluated on a held-out, labeled set with MRR@10, Recall@10, answerable/unanswerable behavior, target commit accuracy, and per-query outcomes.
- No result is described as verified unless the displayed source directly supports the relevant claim.
