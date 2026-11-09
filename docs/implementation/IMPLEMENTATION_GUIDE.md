# GitTrace implementation guide

**Created:** 2026-11-08  
**Companion roadmap:** [`docs/implementation/IMPLEMENTATION_ROADMAP.md`](IMPLEMENTATION_ROADMAP.md)  
**Product review:** [`docs/product/PRODUCT_REVIEW.md`](../product/PRODUCT_REVIEW.md)

This guide translates each roadmap milestone into concrete implementation work for the current checkout. It names the code areas to change, the data model to introduce, the tools/dependencies required, and the evidence needed to consider a revision complete.

## Progress against this guide

Implemented so far: stage/count profiling through `--profile`; exact commit/tree/path/blob occurrences; branch/merge and unchanged-file fixtures; SQLite chunk/embedding/occurrence persistence; model-load deferral; `--index`, `--index-status`, and `--rebuild-index`; nested/global ignore and grammar revision/source-metadata/runtime/compiler fingerprints; SQLite-backed writer serialization; and atomic snapshot replacement. Descendant HEAD advances walk only new commits and reuse known chunks/embeddings. An integration experiment confirmed one new state caused one embedding (versus two on clean rebuild), while an allow-empty commit added occurrence metadata with no embedding/model load. Chunk and occurrence rows matched a clean rebuild after deterministic normalization. The cache fingerprints model and grammar source files by path/revision/size/mtime, not full content hashes; refresh still walks each new commit tree, and timing is only measured on tiny fixtures. The full-repository CPU embedding attempt was stopped because it was taking too long.

## 1. Current repository context

### Existing execution flow

The current CLI in `main.cc` parses flags, constructs `GitTrace`, initializes libgit2 and optionally the embedder, and calls `beginGitTrace()`. `beginGitTrace()` in `GitTrace/gittrace.cpp` walks commits reachable from `HEAD`, walks each commit tree, skips ignored paths, reads text blobs, and deduplicates by blob OID plus path. It then prepares grammars through `TreeSitter::prepare()`, parses each collected `Blob`, extracts syntax spans, and calls `ChunkDB::embed()` for every generated chunk. `ChunkDB` keeps all `Chunk` objects and embeddings in a `std::vector` in memory. `search()` embeds the query when vector search is active, calculates vector/lexical ranks, fuses them, and returns the top results.

The repeated startup work is therefore broader than just embedding: Git history/tree traversal, grammar preparation/build, parsing, chunking, and embedding all happen before the prompt. There is no disk index load/save layer yet.

### Main implementation files

| File | Current responsibility | Expected work |
|---|---|---|
| `main.cc` | CLI options, startup, query loop, output | Add index path/status/refresh/rebuild and profiling controls; make keyword-only mode avoid model initialization. |
| `GitTrace/gittrace.h/.cpp` | Repository setup, history traversal, ignored paths, blob collection, chunk creation | Return explicit commit/tree/path occurrences; scope/ref discovery; timings; incremental work orchestration. |
| `GitTrace/blob/blob.h` | In-memory historical file version | Replace or supplement with persistent content and occurrence types; avoid treating a blob as a complete occurrence. |
| `GitTrace/chunkdb/chunk.h` | `Chunk` metadata, embedding, `ChunkMatch` scores | Add stable chunk/content identity and occurrence associations; retain separate score components. |
| `GitTrace/chunkdb/chunkdb.h/.cpp` | Model lifecycle, embed, chunks, lineage, ranking | Add embedding load/store hooks, metadata serialization boundary, precomputed lexical fields, diagnostics. |
| `GitTrace/chunkdb/retrieval.h/.cpp` | Model-independent lexical tokenizer/BM25 | Reuse normalized terms and corpus statistics; add per-field ranks/diagnostics when evaluation is ready. |
| `GitTrace/utils/treesitter.h/.cpp` | Grammar install/load, parse and extract spans | Cache grammar build artifacts and parse/chunk outputs with explicit version fingerprints. |
| `CMakeLists.txt` | Dependencies and executable/test targets | Add SQLite3 discovery/linking, any new index sources, and test targets. |
| `test.cpp` | C++ integration fixture that creates a temporary Git repository | Expand commits/ref/merge/occurrence tests and persistent-index round trips. |
| `test_cli.py` | CLI argument behavior tests | Cover index path, status, refresh/rebuild, keyword-only mode, and actionable errors. |
| `evaluation/` | A–D scenario text and retrieval captures | Add versioned labeled queries, machine-readable outputs, timings, and benchmark summaries. |
| `README.md` | Build/run and runtime behavior | Document index location, scope, refresh, cache invalidation, offline use, and data disclosure. |

Current build dependencies are CMake 3.10+, C++17, libgit2, Tree-sitter via `pkg-config`, libcurl, and vendored `llama.cpp`. The build currently has a developer-specific `LIBGIT2_ROOT` default. SQLite is **not currently discovered or linked**. CMake's built-in `FindSQLite3` support requires a newer CMake baseline than the project's current 3.10 declaration; either deliberately raise the minimum to a version that supplies `FindSQLite3`/`SQLite::SQLite3`, or use the existing `PkgConfig` approach for `sqlite3` while retaining the current minimum. Check the minimum supported version and imported target name in the CI/build environment before coding.

### Existing tests and evaluation limits

- `GitTraceRetrievalTest` creates a temporary Git repository and verifies current blob/path behavior. It initializes `GitTrace` with embeddings disabled, so it does not validate model embeddings or SQLite persistence yet.
- `GitTraceCliTest` checks command-line behavior through Python.
- `evaluation/experiments/keyword-vector-hybrid-ablation.md` and `evaluation/experiments/query-expansion-ablation.md` document only four diagnostic queries (A–D). Do not treat them as a quality benchmark.
- Existing recent verification: build and two CTest tests passed after the retrieval changes; the new roadmap/guide documents themselves do not change executable code.

## 2. Tools and development setup

### Required for core implementation

- CMake and a C/C++17 compiler.
- Git CLI for creating/verifying temporary test histories; libgit2 remains the application Git API.
- libgit2, Tree-sitter runtime, libcurl, and the existing vendored llama.cpp build.
- SQLite 3 library and development headers. Prefer SQLite's C API from C++ initially; use prepared statements and bind binary embedding buffers with `sqlite3_bind_blob`.
- Python 3 for the existing `test_cli.py` harness; optional Python scripts may be used to generate/evaluate a benchmark, but labels and evaluator behavior should be versioned in the repo.
- A local embedding model only for model-dependent integration and timing runs. Unit tests and persistence round-trip tests should be runnable without loading it.

### Useful commands

```sh
cmake -S . -B build -DLIBGIT2_ROOT=/path/to/libgit2-install
cmake --build build -j2
ctest --test-dir build --output-on-failure
./build/GitTrace --help
git diff --check
```

Record a baseline with the exact repository path, HEAD/ref scope, build ID, flags, model identity, index settings, and warm/cold state. Do not compare timings from different machines/configurations as if they were controlled results.

## 3. M0 — Instrument and freeze the baseline

### Files and changes

1. In `main.cc`, introduce a small run-metrics object and pass it through indexing and query stages, or provide a callback interface to avoid mixing timing output into ranking code.
2. In `GitTrace::beginGitTrace()` time commit walking, tree walking/blob collection, grammar preparation, parsing, chunk extraction, and embedding separately. Count commits visited, eligible occurrences, unique content states, parsed files, spans, chunks, and embedding calls.
3. In `ChunkDB::search()` time query encoding, vector scoring/ranking, BM25 ranking, fusion, and reranking separately. Reranking currently happens above the chunk database in `main.cc`; time the HTTP request there.
4. Add `--profile` or an always-written machine-readable profile file. Prefer explicit JSON output to parsing `dev.log`; keep normal terminal output concise.
5. Write the JSON record atomically and include: schema version, GitTrace build ID, platform, canonical repository path/identity, selected ref/commit, query flags, model fingerprint, counts, stage durations, and result IDs/ranks/scores.
6. Capture cold and repeated runs on a small fixture and one representative larger repository. The first capture establishes timings, it does not need a performance target.

### Tools and tests

- C++ `<chrono>` steady clock for durations; a small JSON library is not currently visible as a direct project dependency. Either add a maintained JSON dependency deliberately or write a narrow escaped JSON serializer with tests. Do not emit invalid JSON by concatenating raw paths/query strings.
- Add a test that parses the emitted profile JSON (Python's standard `json` module is sufficient), verifies required fields, and confirms durations/counts are nonnegative.
- Record the output and exact commands in `evaluation/reports/performance-baseline.md`.

### Completion check

All major startup and query stages have counts and durations; the same invocation/configuration can be compared across cold and warm runs. This milestone must not change ranking logic.

## 4. M1 — Make Git provenance exact

### Data model change

Current `Blob` has one `sha` field that is used as commit provenance, plus path/content/time/author. Since an unchanged state may occur in many commits, one record cannot encode all provenance. Introduce separate records, for example:

```cpp
struct FileContent {
    std::string blob_oid;
    std::string filepath;           // Include if grammar/chunking semantics depend on extension/path.
    std::vector<unsigned char> raw_data;
};

struct FileOccurrence {
    std::string commit_oid;
    std::string tree_oid;
    std::string filepath;
    std::string blob_oid;
    int64_t commit_time;
    std::string author_name;
    std::string author_email;
};
```

Names can be adapted, but content identity and occurrence identity must remain distinct. Chunk text and embedding can be reused; a search result must resolve to a real occurrence in the requested scope.

### Implementation sequence

1. Define scope precisely: first release indexes commits reachable from `HEAD`. Store the resolved HEAD commit and the policy string in the run/index manifest.
2. Change the revwalk callback to add occurrence rows for every commit/tree/path/blob in scope. Deduplicate content or chunk work separately; do not `continue` before recording later occurrences simply because an embedding already exists.
3. Make result selection choose an occurrence explicitly. Default can be the newest reachable occurrence for current-code queries, but the selection policy must be visible and history queries must be able to retrieve older occurrences.
4. Keep `.gitignore` and `.gittraceignore` semantics explicit: the current working tree's rules apply across indexed history (the existing test checks ignored files and rule files). Persist a fingerprint of the effective rule inputs.
5. Expand the temporary test repo in `test.cpp`: two commits with an unchanged file, a changed file, a rename, two paths with identical content, a side branch, a merge, and ignored paths. Verify each expected occurrence's full commit SHA, path, text and line range.
6. Compare the implementation's occurrence set against Git CLI commands in the fixture (`git rev-list`, `git ls-tree -r`, and `git show`) as an independent test oracle. Quote paths safely or use libgit2 for all values once commands become complex.
7. Make lineage output explicitly heuristic. Add relation confidence/method and a ranking flag that can disable lineage adjustment. Do not use timestamp ordering alone to imply parent-child lineage.

### Tools and tests

- `git` CLI in fixture tests, libgit2's revwalk/tree/diff APIs in production.
- `ctest --test-dir build --output-on-failure`; extend current `GitTraceRetrievalTest`.
- Add deterministic branch/merge fixtures; ensure the test does not assume a particular default branch name.

### Completion check

Every displayed result has a valid commit/tree/path/blob occurrence. Duplicate content can reuse bytes/embedding without losing occurrences. The declared scope matches exactly the refs used by traversal.

## 5. M2 — Add a versioned SQLite index

### Proposed first schema

Use a single repository-local `.gittrace/index.sqlite` initially. Keep the database out of history with `.gitignore` documentation/rules if appropriate. Use explicit IDs and foreign keys. One workable initial schema is:

```sql
PRAGMA foreign_keys = ON;

CREATE TABLE manifest (
    key TEXT PRIMARY KEY,
    value TEXT NOT NULL
);

CREATE TABLE commits (
    commit_oid TEXT PRIMARY KEY,
    parent_oids TEXT NOT NULL,
    commit_time INTEGER NOT NULL
);

CREATE TABLE contents (
    blob_oid TEXT NOT NULL,
    filepath TEXT NOT NULL,
    PRIMARY KEY (blob_oid, filepath)
);

CREATE TABLE chunks (
    chunk_id INTEGER PRIMARY KEY,
    blob_oid TEXT NOT NULL,
    filepath TEXT NOT NULL,
    chunk_key TEXT NOT NULL,
    start_line INTEGER NOT NULL,
    end_line INTEGER NOT NULL,
    syntax_type TEXT NOT NULL,
    syntax_ordinal INTEGER NOT NULL,
    source_text BLOB NOT NULL,
    embedding BLOB,
    embedding_dim INTEGER,
    FOREIGN KEY (blob_oid, filepath) REFERENCES contents(blob_oid, filepath)
);

CREATE TABLE occurrences (
    commit_oid TEXT NOT NULL REFERENCES commits(commit_oid),
    tree_oid TEXT NOT NULL,
    filepath TEXT NOT NULL,
    blob_oid TEXT NOT NULL,
    PRIMARY KEY (commit_oid, filepath)
);

CREATE TABLE chunk_occurrences (
    commit_oid TEXT NOT NULL,
    filepath TEXT NOT NULL,
    chunk_id INTEGER NOT NULL REFERENCES chunks(chunk_id),
    PRIMARY KEY (commit_oid, filepath, chunk_id),
    FOREIGN KEY (commit_oid, filepath) REFERENCES occurrences(commit_oid, filepath)
);
```

This is a starting point, not a mandated final schema. If a chunk's stable identity is based on parser output rather than blob/path, store that identity explicitly. Avoid storing a giant list of parents as an opaque text field in the production schema; a normalized `commit_parents(commit_oid,parent_oid,parent_index)` table is preferable. Add indices for commit, path, blob, chunk, and token lookup. Decide whether source text is held in SQLite or reloaded from Git objects; source-in-DB simplifies exact output and avoids depending on pruned objects, while Git OIDs can still be stored for identity.

### Manifest contents and invalidation

At minimum store:

- Index schema version and GitTrace build/indexer version.
- Canonical repository path plus stable Git repository identity.
- Indexed ref policy and resolved refs/HEAD commits.
- Embedding model file fingerprint (or an explicitly stable model revision), dimension, pooling/normalization, and corpus input format/version.
- Chunker version and settings; grammar manifest fingerprint plus each grammar source revision and Tree-sitter runtime ABI.
- Ignore-rule fingerprint and current ignore policy.
- Lexical tokenizer/BM25 version and lineage algorithm version for derived data.

Use exact manifest key/value comparison. Initially invalidate/rebuild the affected full index if any core fingerprint is unknown or mismatched. Never silently load vectors from a different model or chunking version. Later, separate invalidation domains so occurrence changes do not force re-embedding.

### C++ integration steps

1. Add `GitTrace/index/index_store.h/.cpp` or similar. Keep raw `sqlite3*` handles out of `GitTrace` and `ChunkDB`; expose typed operations (open, read manifest, list reusable chunks, write staging snapshot, commit, rollback).
2. In `CMakeLists.txt`, discover SQLite and link it to both application and tests. Verify portability on supported CMake versions/platforms.
3. Add RAII wrappers for database, statements, and transactions. Check all SQLite return codes and include `sqlite3_errmsg()` in errors.
4. Use prepared statements for every value. Bind embeddings with `sqlite3_bind_blob(..., SQLITE_TRANSIENT)` and validate byte count equals `dimension * sizeof(float)` on load. Do not persist compiler-dependent structs.
5. Add an index lock. SQLite locking alone handles transactions, but a GitTrace-level lock avoids two indexers redundantly parsing/embedding the same repository at once. Use platform-appropriate lock implementation or a well-supported library; document stale lock recovery.
6. Build into a transaction/staging generation, validate counts and manifest, then atomically make it current. A crash must leave either the last good generation or a clearly incomplete generation that is rejected.
7. Add CLI flags such as `--index PATH`, `--index-status`, and `--rebuild-index`; define their behavior and error messages in `main.cc` and `test_cli.py`.
8. Keep the first version coarse: if no compatible manifest is present, rebuild the whole index. Implement fine-grained incremental updates only after M1/M2 round trips are correct.

### Tests

- In-memory or temporary SQLite DB schema creation and migration.
- Chunk/source/embedding serialization round trip with dimensions 0/normal/invalid and binary byte values.
- Full clean build then load, compare ordered result chunk IDs, scores within tolerance, and occurrence metadata.
- Manifest mismatch tests for model, dimension, grammar/chunker version, ignore rules, ref scope, and schema.
- Transaction interruption simulation: force a write failure before commit and ensure the previous index is still loadable.
- Concurrent writer/lock test if platform support is included.
- Run persistence tests without the embedding model by injecting fixed test embeddings through a narrow test API; do not require GPU/model download for normal CTest.

### Completion check

The second unchanged run does not load the embedding model for corpus encoding or call `ChunkDB::embed()` for source chunks. Search results from loaded and clean indexes match. Stale/corrupt indexes fail clearly and can be rebuilt.

## 6. M3 — Incremental refresh and resumability

### Commit and file-state algorithm

1. Read indexed refs and commit IDs from the manifest/database.
2. Resolve current selected refs. Identify new reachable commits by walking from each selected tip while excluding already indexed commits. Store parent edges and every occurrence in those commits.
3. For each new occurrence, lookup the reusable `(blob_oid, path/parser identity)` content/chunk records. If present and its chunker/grammar fingerprints match, add occurrence links only. If not, read blob, parse, chunk, and embed once.
4. Update ref tips only after all new commit and occurrence records are durable. If interrupted, rerun should detect missing commits/rows and continue idempotently.
5. For changed ignore rules or ref scope, recompute occurrence visibility. Do not merely reuse the old filtered occurrence set.
6. For grammar/chunker/model changes, invalidate only the dependent layer when safe: parser/chunker changes rebuild spans/chunks; model/embedding-format changes rebuild vectors; lexical/tokenizer changes rebuild lexical fields; lineage changes rebuild lineage data. During early implementation, choose safe full rebuilds when dependency boundaries are uncertain.

### Grammar and parser caching

`TreeSitter::install_and_load()` currently clones source if missing, writes a generated CMake project, runs CMake configure/build, and loads a dynamic library. The code does not currently have a clear source revision or build fingerprint. Add:

- A pinned grammar commit/revision in the grammar manifest.
- Build fingerprint: grammar revision, source subdirectory, export symbol, Tree-sitter runtime ABI, platform/architecture, compiler identity/version, and relevant build flags.
- Reuse an artifact only when the fingerprint matches; otherwise rebuild into a temporary directory and replace only after successful load/validation.
- Explicit offline mode: if grammar source/artifact is unavailable, report unsupported extensions and proceed only according to documented policy.
- Optional parse-span cache keyed by blob OID + extension/parser identity + grammar revision + span-extractor version. Persist extracted spans as data before chunk embedding; this enables a model change without reparsing every file.

### CLI and interruption behavior

- `--refresh-index`: compare current selected refs/settings, update if stale, then enter query mode.
- `--rebuild-index`: discard/rebuild safely; preserve the old DB until the new one passes validation.
- `--index-status`: report indexed refs/HEAD, manifest compatibility, counts, and stale reason without loading the embedder.
- Add progress phases and counts, plus cancellation handling if practical. Cancellation must leave the prior valid index intact.

### Tests and completion check

Create a two-run fixture: first run indexes two commits; second adds a commit that only changes one file; refresh should add all commit/path occurrences but parse/embed only genuinely new eligible content. Add a commit that reuses a known blob, a change to ignore rules, and a simulated interrupted update. Verify expected incremental counters and compare final index against a clean rebuild.

## 7. M4 — Build a useful retrieval benchmark

### Dataset shape

Add `evaluation/queries.jsonl` (or a versioned directory) with one JSON object per query, e.g.:

```json
{
  "id": "repo1-current-001",
  "repo": "repo1",
  "query": "Where is the retry delay increased after repeated failures?",
  "scope": "current",
  "answerable": true,
  "relevant": [
    {"path": "src/retry.cpp", "commit": "<full-sha>", "start_line": 42, "end_line": 59, "grade": "direct"}
  ],
  "split": "train"
}
```

Use stable query IDs and immutable dataset versions. Validate path, full commit, and line span against the named repository revision. Avoid storing only expected phrases: evaluation should check evidence identity and relevance labels.

### Collection and split

1. Collect at least 30 natural developer questions across several languages/repos, including misspellings/partial identifiers, behavior-first language, current vs historical, path constraints, and no-answer queries.
2. Have a human label direct evidence and related context separately. Require a second review for ambiguous cases.
3. Split by query and preferably by repository or task family where possible, before any synonym/rank tuning. Keep the test split hidden from heuristic development.
4. Preserve A–D as regression diagnostics, not as the held-out quality score.

### Runner and metrics

- Add a Python evaluation runner that invokes GitTrace in one-shot mode or, preferably, a stable JSON output mode. Save raw runs and computed metrics with GitTrace build ID and index manifest.
- Conditions: vector, keyword, hybrid, hybrid + expansion, each with/without reranker only where configured; same top-k and query wording. Add `git grep` only for working-tree lexical tasks and state its scope limitation.
- Compute MRR@10, Recall@5/10, Precision@5, direct-evidence rate, target commit accuracy, answerable/no-answer false-positive behavior, and latency. Include per-query rows.
- Add a diagnostic mode to expose lane ranks, matched tokens, expansion, lineage penalty, and fusion score. Do not overload normal result prose.

### Completion check

Every metric can be traced to a query/result/label. Rank and threshold changes use the train split only; held-out results are reported after freezing the change.

## 8. M5 — Improve ranking and evidence context

Implement only after M4 has a stable split.

### Contextual chunk construction

1. Extend the Tree-sitter span extraction to report enclosing symbol and parent syntax context. Inspect `extractSpans()` and query patterns in `GitTrace/utils/treesitter.cpp`.
2. Keep two strings: exact source snippet for display and an embedding/search document assembled from path, symbol, syntax type, signature, selected comments/docstring, and source text.
3. Bound embedding input by the model tokenizer/context size; define deterministic truncation and line mapping. Avoid adding entire files to every chunk.
4. Add fixtures for tiny declarations, free functions, methods, configuration/global constants, and large spans. Ensure snippet lines remain exact.

### Ranking diagnostics and fielded retrieval

1. Refactor `Chunk` to retain separate searchable fields: source, filepath, symbol, syntax kind, and optionally commit message/author.
2. Extend `retrieval.cpp` to compute BM25 per field or allow weighted field scores, keeping exact identifiers/path fragments as separate high-precision signals.
3. Add per-lane rank and term-match trace data to a JSON/debug result format.
4. Tune weights/fusion only on training queries; compare vector-only, BM25-only, hybrid, and each lineage variant.
5. Evaluate query expansion on held-out cases. Do not add aliases specifically to rescue held-out misses.
6. Measure retrieval candidate recall before reranker and choose candidate over-fetch based on latency/recall curves.
7. Add a no-strong-match cutoff only after calibration. Include unanswerable cases and choose thresholds based on stated false-positive cost.

### Completion check

Any claimed improvement appears on held-out data and does not hide per-query regressions. Results provide exact evidence and are not framed as verified by the reranker.

## 9. M6 — Portability and user-facing behavior

### Build/runtime configuration

1. Remove `/Users/stovetop/lib/libgit2-install` as the source default. Let CMake use system package discovery; retain `LIBGIT2_ROOT` as an optional override.
2. Add explicit settings for model path, grammar resources/cache, SQLite index path, and repository path (CLI flags, environment variables, or a documented config file with clear precedence).
3. Resolve defaults relative to a stable app/config/cache directory rather than the process CWD. Continue supporting `--repo` independently of where GitTrace launches.
4. Pin grammar revisions in `GitTrace/rsc/grammar.txt` or a lockfile; document build/network requirements and offline cache behavior.
5. Test a clean configure/build on macOS and Linux without developer-local paths.

### UX, privacy, and docs

- Make index status, refresh, rebuild, stale reason, history scope, ignore semantics, and database location visible.
- Clarify score names and that similarity/reranker confidence are not calibrated truth probabilities.
- Keep local retrieval useful when Laya is absent. Before Gemini use, disclose query/source snippets being sent to the external API and possible cost.
- Document cache deletion/rebuild and whether source text is stored locally in the index.
- Validate README commands on clean environments and record tested platform/tool versions.

### Completion check

A new user can configure/build, perform a local search without a reranker, build and reuse the index, refresh after commits, recover from a stale index, understand which refs are covered, and understand any data sent externally.

## 10. Cross-cutting engineering rules

- Keep unit tests independent from the embedding model and network. Use a fixed embedding provider/test seam for index serialization and ranking tests.
- Treat a saved embedding as valid only for the exact model and corpus encoding fingerprint that produced it.
- Do not use `SELECT *` or raw C++ memory dumps as persistent formats; name columns and schema versions explicitly.
- Use prepared SQL statements, transactions, input-size bounds, and explicit return-code handling.
- Make index updates idempotent and crash-safe. Never replace the last valid DB before a new generation validates.
- Preserve deterministic IDs/tie-breaking so clean and cached runs can be compared.
- Collect metrics before adding concurrency. llama.cpp contexts and Tree-sitter parser objects should not be assumed thread-safe.
- Do not add ANN, a daemon, or custom storage format until profile data identifies a bottleneck SQLite/linear scan cannot meet.

## 11. Suggested pull-request-sized slices

1. **Instrumentation only:** metrics type, JSON profile, baseline doc, tests.
2. **Occurrence model only:** C++ structs and history fixture expansion; no SQLite yet.
3. **SQLite schema and round-trip:** typed store, manifest, fixed embedding tests, build integration.
4. **Load/rebuild CLI:** cold build and warm load; exact result equivalence.
5. **Incremental commit refresh:** idempotent occurrence insertion and new-content processing.
6. **Grammar cache fingerprint:** pin source and avoid unnecessary configure/build.
7. **Evaluation runner/data:** labels, held-out split, metrics, raw outputs.
8. **Context/ranking experiments:** one change per experiment with before/after reports.
9. **Portability/release:** config paths, clean builds, refreshed documentation.

Keep each slice reviewable and avoid mixing retrieval tuning with storage correctness in the same change.
