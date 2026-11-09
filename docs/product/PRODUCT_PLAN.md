# GitTrace product plan

**Status:** Draft for discussion  
**Prepared:** 2026-11-08  
**Product scope:** Local, source-backed search across a Git repository and its history

## Work started from this plan

**Progress update: 2026-11-08**

- Fixed blob identity to include both object ID and repository-relative path. The integration fixture now covers identical content at two paths and a rename across commits, including the selected commit provenance for each path. A full commit-to-tree occurrence map is still needed for exact queries about every revision.
- Added a fixture for the current ignore policy: the working tree's `.gittraceignore` and `.gitignore` rules filter paths throughout the indexed history.
- Split raw cosine, lineage-adjusted similarity, and retrieval score in `ChunkMatch` and CLI output. The reranker receives raw cosine. Reranker failure now names the selected retrieval mode.
- Extracted lexical tokenization and BM25 into a model-independent retrieval component. Deterministic A–D lexical fixtures cover token normalization and weighted expansion without loading the embedding model.
- Expanded CLI integration checks for retrieval-mode options and invalid values. `cmake --build build -j2` and `ctest --test-dir build --output-on-failure` pass (2 registered CTest tests).
- Added machine-readable stage timing/count output (`--profile`), complete occurrence lists for indexed path/blob states, branch/merge/unchanged-file fixtures, and an initial SQLite whole-index cache. A one-chunk cold/warm smoke test confirmed a cache hit; incremental refresh and broader invalidation remain unimplemented.

Phase 0 is underway, not complete: branch/merge coverage and lineage correctness remain. The full multi-repository retrieval benchmark and persistent incremental index have not been built yet.

## Product goal

Help a developer find the implementation or historical change they have in mind, even when they remember its behavior but not its identifier. Results should make it easy to verify the evidence: show the path, line range, full commit ID, and whether the result is current or historical. If GitTrace cannot find adequate evidence, it should say so instead of presenting an unrelated top-ranked chunk as an answer.

## Five refinement passes

This plan was revised five times. Each pass incorporated evidence that changed the priorities.

1. **Surface audit — from feature list to user job.** The project already has a CLI, repository scanner, Tree-sitter chunker, local embeddings, lexical/vector retrieval, and optional Laya or Gemini reranking. The product job is not “add more models”; it is find and verify code evidence from an imperfect developer query.
2. **Evaluation audit — from ranking ambition to measured relevance.** On the four A–D queries, vector, keyword, hybrid, and opt-in expansion all produced misses. Expansion moved A to rank 1 and helped B in keyword-only mode, but did not retrieve C and pushed D out of hybrid’s top 3. A four-query hand-curated set is useful for debugging, not evidence of general quality. See [the mode comparison](../../evaluation/experiments/keyword-vector-hybrid-ablation.md) and [the expansion ablation](../../evaluation/experiments/query-expansion-ablation.md).
3. **Indexing audit — from model tuning to evidence integrity.** Blob deduplication currently keys only on the Git blob ID. Identical content at two paths can therefore lose one path’s metadata. Chunks often contain source text without enclosing symbol or file context. Historical matching uses syntax kind, ordinal, timestamp, and embedding similarity as a heuristic; it is not a Git rename or parent/merge model. These can make a good rank score point to the wrong evidence, so fix and test indexing invariants before tuning ranking further.
4. **Runtime audit — from “too much history” to repeatable local use.** The measured repository indexed 224 chunks; it is not too large for the current search algorithm. The bigger product friction is rebuilding and re-embedding on each run, loading the local model before the prompt, and possibly checking out/building Tree-sitter grammars on first use. Persistence and incremental updates should precede a separate background service; a daemon adds lifecycle and synchronization work without addressing correctness by itself.
5. **Trust and release audit — from prototype to usable tool.** At the start of the review, the CLI always returned a top-*k* list, even when every result was irrelevant. It labeled a lineage-decayed score as cosine and described fallback results as vector results regardless of mode. The first two interface issues have now been corrected; calibrated no-result behavior, evidence-aware output, deeper ranking diagnostics, and explicit privacy/build expectations remain. Laya/Gemini can only judge submitted candidates and cannot recover a missing target; Gemini also sends query and source snippets to an external service.

## Current product evidence

| Area | Current state and implication |
|---|---|
| Target workload | Local repository search, including source versions reachable from `HEAD`; current behavior versus historical behavior is not yet a clear user-selectable distinction. |
| Index correctness | Blob identity includes object ID and repository-relative path. Blob and chunk records retain commit/tree/path/blob occurrences; fixtures cover identical content, rename, unchanged state, a side branch, and a merge. Scope is commits reachable from `HEAD`, not all refs. |
| Chunking | Tree-sitter queries mainly capture functions and classes. Large spans are split at a byte limit, while small declarations can be very thin. Embedded text does not consistently include enclosing symbol or path context. |
| Retrieval | Current modes are vector, BM25 keyword, and reciprocal-rank hybrid. An opt-in synonym map is hand-authored from A–D. It improved selected ranks, did not solve C, and hurt D under hybrid fusion. Keep it experimental. |
| Historical relevance | Lineage depth is inferred heuristically from same-path syntax chunks. It can confuse similarly shaped functions, and historical copies can crowd a current-code query. |
| Result trust | CLI now reports raw cosine, lineage-adjusted similarity, and retrieval score separately, and fallback text reflects the selected mode. Rank components are not yet shown; there is no calibrated relevance threshold or robust “no useful match” state. Reranking sees a bounded candidate list, so it cannot fix low recall upstream. |
| Runtime and install | A SQLite whole-index cache now reuses chunks/embeddings when its current manifest matches; changed HEAD or tracked configuration triggers a full rebuild. It does not yet refresh incrementally or fingerprint every global/grammar build input. Model and grammar paths remain working-directory-relative; first-use grammar setup may need network/compiler access. CMake retains a developer-specific default libgit2 prefix. |
| Validation | History and CLI integration tests pass, and deterministic lexical fixtures cover A–D terms and expansion. Full-repository A–D runs are captured for keyword, vector, hybrid, and expanded keyword/hybrid modes in `evaluation/`. This remains a four-query diagnostic, not a representative quality benchmark. |

## Roadmap

### Phase 0 — Make indexed evidence correct

**Priority:** First. Ranking cannot compensate for missing or incorrect path, commit, or chunk metadata.

- Deduplicate content by object ID, but represent searchable occurrences with path and commit/tree membership. If unchanged file states are collapsed, retain the set or range of commits where the state occurs and define which commit is shown by default.
- **Initial ref policy:** index commits reachable from the current `HEAD`. Add all local branches and tags as an explicit later option. Record the actual ref and commit associated with each result; add branch/merge fixtures before broadening the policy.
- **Initial ignore policy:** the current working tree’s `.gittraceignore` and Git ignore rules apply to every indexed historical snapshot. This policy is implemented and tested; make it visible in user documentation.
- Add deterministic fixtures for identical content at two paths, renames, unchanged files across commits, ignored paths, branches, and merge commits. Assert path, line range, full commit SHA, and source text—not only blob counts.
- Replace heuristic claims about lineages with explicit confidence/relationship metadata. Until the lineage matcher is validated, present it as an inferred relation and avoid letting its decay silently masquerade as cosine similarity.

**Exit gate:** Every fixture returns all expected path/commit occurrences, and no displayed source can be joined to the wrong path or commit.

### Phase 1 — Build a trustworthy retrieval evaluation loop

**Priority:** Before further model or synonym tuning.

- Create a versioned set of at least 30 developer-authored queries over multiple repositories and languages. Include natural descriptions, remembered identifiers, misspellings, current-code questions, historical questions, and queries whose answer is absent.
- Have a reviewer label relevant evidence spans and commit IDs; mark direct evidence separately from surrounding context. Keep a held-out set that is not used to design expansion rules.
- Run vector-only, keyword-only, hybrid, and reranker conditions on the same index, candidates, query wording, and result budget. Include a Git grep baseline for working-tree keyword tasks and clearly label the history-scope difference.
- Report MRR, Recall@5/10, Precision@5, target commit accuracy, no-answer behavior, and latency. Keep individual query outcomes visible; a single aggregate score must not hide C-like misses.
- Add a diagnostic option that prints vector rank, lexical rank, matched terms, expansion terms and weights, lineage adjustment, and final fused score. Rename `cosine` in output or report true raw cosine separately from decay-adjusted similarity.
- Add deterministic tests for suffix normalization, identifier splitting, expansion on/off, score ordering, and tie behavior. Keep model-dependent quality tests optional and separate from the ordinary fast test suite.

**Proposed release gate:** On held-out queries, at least 85% of answerable questions have a direct evidence chunk in the top 10, with MRR@10 at least 0.7; unanswerable questions should not be represented as confident matches. Revisit these thresholds after collecting the first credible evaluation set.

### Phase 2 — Improve retrieval and evidence presentation

- Make chunks carry useful context: repository-relative path, enclosing symbol, syntax kind, and source lines. Use AST-aware windows with a bounded token budget and a small overlap; include globals/configuration/docstring-like constructs that the current syntax queries omit.
- Keep code, path, symbol, and commit metadata as separate searchable fields. Tune BM25 field weights on training queries instead of mixing everything into one text field.
- Keep exact identifiers and path fragments high precision. Treat stemming and synonym expansion as separate lower-weight signals. Do not grow a list of A–D-specific aliases; test general normalization on held-out repositories.
- Calibrate hybrid fusion and any relevance cutoff against labeled queries. Prefer showing fewer defensible results or “no strong match” over filling every top-*k* slot with unrelated chunks.
- Add explicit query controls for current code versus history, path/language filters, and commit/ref scope. Do not merge historical results away when the user asks about change history.
- Keep reranking as an optional second stage. Measure candidate recall first, allow a configurable over-fetch budget, and ensure a failed reranker falls back to the selected retrieval mode with an accurate message.

**Exit gate:** The held-out relevance gate passes, every result’s provenance is correct, and query expansion does not materially regress any evaluation slice.

### Phase 3 — Make repeated local use fast and reliable

- Persist the local index (SQLite is a reasonable first candidate) with repository identity, object IDs, paths, commit/ref metadata, chunks, embeddings, and an index-version manifest.
- Invalidate or rebuild only the affected records when the model, grammar, chunking rules, or ignore policy changes. On a second run at unchanged `HEAD`, reuse stored embeddings rather than embedding the same chunks again.
- Make indexing resumable and safe after interruption. Use a repository lock and atomic index updates so a query never reads a half-written index.
- Show indexing phases and counts, permit cancellation, and distinguish “index current” from “indexing.” Measure cold and warm scans on small, medium, and large repositories before promising performance.
- Keep search and indexing in one process with persistent storage at first. Add a background service only if concurrent clients, long-running indexing, or measured CLI startup still justify it. Add ANN/vector-database machinery only when corpus benchmarks show linear search is a bottleneck.

**Exit gate:** An unchanged second scan performs no source re-embedding; interrupted updates recover; warm-query and cold-index timings are measured on representative repositories.

### Phase 4 — Ship a clear, privacy-preserving developer experience

- Present a result with path, line range, full commit SHA, date/ref, current-versus-historical label, snippet, and a concise reason it matched. Link the score label to what was actually measured.
- Add “useful / not useful” feedback and optional query refinement; save feedback locally for evaluation only unless the user explicitly exports it.
- Keep local embeddings and search as the default. Gemini remains explicit opt-in with a clear notice that query and source text leave the machine. Never log API keys or silently send source to a remote model.
- Make Laya/Gemini availability, timeout, candidate limits, and fallback behavior explicit. A reranker should not be presented as factual verification.
- Remove developer-specific absolute dependency paths. Pin grammar revisions, expose model and grammar locations in configuration, document offline behavior, and add clean macOS/Linux build checks before wider distribution.
- Add an installation path and a short “first search” guide after clean-environment builds are reliable. Keep model acquisition and licensing information clear.

**Exit gate:** A new user can build/install, index a sample repository, understand the results, and complete a search without hidden network requirements or misleading fallback behavior.

## Product decisions to resolve

1. **History scope — initial policy:** Search commits reachable from the current `HEAD`; add an explicit all-local-refs option after branch and merge coverage is tested.
2. **Ignore semantics — decided for the first release:** The current working tree’s `.gittraceignore` and Git ignore rules govern all indexed historical snapshots. This matches the existing implementation and is now covered by the history fixture. Revisit historical per-commit rules only if users need that distinction.
3. **Default result mode:** Keep hybrid as the default only after it clears the held-out quality gate. Keep expansion off by default until it does not regress held-out slices.
4. **Remote reranking:** Keep external providers opt-in and separate from the local core; source disclosure must be visible before use.

## Explicit non-goals for the next release

- Training or fine-tuning Laya/Gemini.
- Building a multi-user hosted service or mandatory background daemon.
- Claiming that an LLM reranker verifies correctness.
- Optimizing for huge monorepos before profiling persistent, incremental indexing.
- Adding more hand-written synonyms without held-out evaluation.

## Immediate next actions

1. **Done:** add duplicate-content, rename, and ignore-rule fixtures; preserve distinct `(blob ID, path)` records.
2. **Done:** separate raw cosine, lineage-adjusted similarity, and retrieval score; correct the mode-specific fallback message.
3. **In progress:** deterministic model-independent A–D lexical fixtures and full-repository ranked-output captures are in place. Next add held-out developer queries and machine-readable result records.
4. **Done for the current diagnostic:** rerun vector, keyword, hybrid, and expansion conditions with one-shot CLI queries and save full outputs. The results show A/B improvements from expansion, no C retrieval, and a D regression in expanded hybrid; expansion remains opt-in. See the two ablation reports in `evaluation/`.
5. Implement a commit/tree occurrence map, then add branch and merge fixtures before designing the persistent index schema.
