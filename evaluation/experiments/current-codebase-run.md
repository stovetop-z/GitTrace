# GitTrace current-codebase retrieval run

Run date: 2026-11-06  
Repository HEAD: `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`  
Build: `GitTrace 0.1.0+d77c3ccebcd2-dirty`  
Mode: vector search, `--no-laya`; Laya/Gemini reranking was not used.

## Default full-repository run

Command: `./build/GitTrace --repo . --top-k 10 --no-laya`

GitTrace parsed 27 unique file versions, reported 8 unsupported or invalid, and indexed 176 semantic chunks. The default index returned unrelated declaration and `.gitignore` chunks for Scenarios A–C. Scenario D's current fallback implementation appeared at rank 8; historical versions appeared at ranks 2 and 3.

| Scenario | Target | Default result |
|---|---|---|
| A | `GitTrace/chunkdb/chunkdb.cpp:238-239` | Not in top 10 for “What calculation downweights chunks from older revisions during search?” |
| B | `GitTrace/gittrace.cpp:49-58, 132-133` | Not in top 10 for the repository-local ignore question |
| C | `laya_service.py:137-143` | Not in top 10 for the Gemini candidate-instruction question |
| D | `main.cc:174-175` | Current implementation in `main.cc:129-182` at rank 8, similarity `0.3181`, commit `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`. Historical matching versions appeared at ranks 2 and 3. |

Representative top-three results from the default run:

- **A:** `GitTrace/gittrace.h:10` (`0.5156`, commit `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`); `GitTrace/chunkdb/chunkdb.cpp:177` (`0.5090`, same commit); `.gitignore:1-4` (`0.4669`, commit `f2d7368b61b94adf3f9b7d7fd07896133cb71459`).
- **B:** `GitTrace/gittrace.h:10` (`0.4637`, commit `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`); `.gitignore:1-4` (`0.4541`, commit `f2d7368b61b94adf3f9b7d7fd07896133cb71459`); `.gitignore:1-5` (`0.3948`, commit `e2db25d8eb0fc8f3ea9263a531b2e72184eb11ae`).
- **C:** `GitTrace/gittrace.h:10` (`0.4171`, commit `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`); `.gitignore:1-4` (`0.3669`, commit `f2d7368b61b94adf3f9b7d7fd07896133cb71459`); `GitTrace/utils/treesitter.h:10` (`0.3644`, commit `e2db25d8eb0fc8f3ea9263a531b2e72184eb11ae`).
- **D:** `GitTrace/gittrace.h:10` (`0.3997`, commit `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`); historical `main.cc:75-131` (`0.3855`, commit `7c1352366b18c9a85aa07e624cffa11e43b1cd1b`); another historical `main.cc:75-131` (`0.3615`, commit `0674f66369180839f1a13e6878b2d9dfdd8f9cdc`).

## Isolated code-focused experiment

To test whether low-signal files were crowding results, I ran the same current Git history in a temporary clone with these `.gittraceignore` rules:

```gitignore
.gitignore
GitTrace/gittrace.h
GitTrace/utils/treesitter.h
GitTrace/rsc/grammar.txt
README.md
LICENSE
requirements-*.txt
test.cpp
test_cli.py
training/
```

This indexed 166 semantic chunks. These filters were used only in the temporary evaluation clone; they were not added to the project.

| Scenario | Query used in the filtered run | Retrieved target |
|---|---|---|
| A | “Where is the exponential decay applied to cosine similarity using `lineage_depth`?” | `GitTrace/chunkdb/chunkdb.cpp:230-246`, rank 10, similarity `0.2856`, commit `d77c3ccebcd22970ed4b21c58d35a810b60fa72b` |
| B | “Where are paths checked against ignore rules while GitTrace walks historical commit trees?” | No verified hit. A rank-4 `gittrace.cpp` chunk came from commit `9d755e01956de1d5d8491876715b05d0f719bef5`, which predates the `.gittraceignore` implementation. Even the exact `.gittraceignore` query did not return the implementing commit. |
| C | “Can a retrieved code snippet override Gemini reranking instructions?” | `laya_service.py:115-174`, rank 3, similarity `0.3144`, commit `d77c3ccebcd22970ed4b21c58d35a810b60fa72b` |
| D | “If the relevance reranker cannot be reached, what does the CLI display?” | Current code `main.cc:129-182`, rank 4, similarity `0.3181`, commit `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`; historical matching version ranked 1. |

The filtered experiment improves the top-10 results for A, C, and D. B remains a miss; the rank-4 result is a false positive once its commit is checked. Do not report it as a successful retrieval.

## Interpretation: current retrieval is failing these scenarios

The displayed top-three results are not meaningful matches for A–D. A–C return generic declarations or ignore-file contents, and D returns a generic declaration plus historical implementations. The current D target is only rank 8. The top-three run therefore demonstrates a failure of the unreranked vector baseline, not a successful retrieval result.

The scores are raw cosine similarities, not relevance probabilities. `ChunkDB::search` sorts every indexed chunk and returns the first `k` without a relevance cutoff, so it will display a top result even when every candidate is unrelated. The CLI run used `--no-laya`, so it did not exercise reranking. Chunks are embedded from each Tree-sitter span's text alone; short spans such as `class TreeSitter` have little surrounding context. The run log also contains repeated llama.cpp warnings that embedding calls supplied no output-token flags and the runtime overrode them. These observations identify likely areas to investigate, but they do not establish whether the dominant cause is model configuration, pooling, chunk granularity, or the scoring method.

## Embedding protocol audit

The model in this checkout is BGE-Code-v1, 1536 dimensions, with llama.cpp reporting pooling type `3` (last-token pooling). BAAI's model card demonstrates retrieval with queries formatted as `<instruct>{instruction}\n<query>{query}` and corpus passages embedded without that query wrapper. GitTrace previously embedded both query and code chunks as plain text. The documented query/task format was therefore missing and is a concrete, high-confidence mismatch. [BAAI's model card](https://huggingface.co/BAAI/bge-code-v1/blob/main/README.md) shows this query/corpus distinction.

I added a query-only instruction wrapper in `ChunkDB::search` using a repository-code retrieval instruction. On a 10-chunk focused fixture, it moved scenario A's target from outside the top three to rank 1 and kept B–D's targets at rank 1; see [instructed-query-ablation.md](instructed-query-ablation.md). The later full-repository runs below supersede the initial canceled comparison. The ranks in the first section remain the original raw-query baseline; [scenario-questions.txt](../scenarios/scenario-questions.txt) labels them accordingly.

The repeated `embeddings required ... overriding` messages have a specific explanation in the vendored llama.cpp: embedding mode requires output for all input tokens, while `llama_batch_get_one` provides no per-token output flags. llama.cpp overrides the flags and continues. The model's recorded last-token pooling and `llama_get_embeddings_seq` access are consistent with BGE-Code-v1's embedding path, so this warning is noisy but is not evidence by itself that the vectors are corrupt. The canonical llama.cpp embedding example marks all batch tokens as output and then reads the pooled sequence embedding. [llama.cpp embedding example](https://github.com/ggml-org/llama.cpp/blob/master/examples/embedding/embedding.cpp).

## Remaining retrieval issues to address

- Keep query and corpus encoding aligned with the model card, then measure before/after against the same gold code spans.
- Add useful context to tiny syntax spans (at least file path, enclosing symbol, and syntax kind) so generic leaves are not embedded alone.
- Measure the current lexical/vector hybrid against a vector-only baseline. The first hybrid implementation uses reciprocal-rank fusion, but results below show it has not resolved all four scenario misses.
- Handle history explicitly in ranking, such as deduplicating equivalent chunks or preferring the current version when the query is about current behavior.
- Add a relevance cutoff only after calibrating it on labeled queries; a cutoff can suppress junk but cannot recover a missed target.

The next evaluation should compare the original query encoding, the BGE-Code-v1 instructed query encoding, and a lexical/hybrid condition using the same scenario queries, gold spans, repository revision, and result budget. Record target reciprocal rank, precision at *k*, and whether the returned snippet actually supports the answer. Do not describe the current vector run as useful retrieval.

## Reproduction notes

- `./build/GitTrace --version` reports the source revision and marks this uncommitted build `dirty`.
- Search results now print full 40-character commit hashes.
- The default full-repository run is the product baseline. The filtered run is a separate experimental condition and should be labeled as such in any survey.

## Follow-up: instructed hybrid retrieval and chunk-size check

The earlier wording above predates the later full-repository runs. The instruction wrapper and a lexical/vector reciprocal-rank-fusion pass are now implemented in `ChunkDB::search`. Full-repository runs use GPU inference when launched with permission to access Metal; the repository size is modest (35 unique text blobs, 27 parsed file versions, 176 semantic chunks before the latest chunk-size change), so size is not the primary cause of the poor results.

The hybrid top-10 output in [full-repository-hybrid-top10.txt](../full-repository-hybrid-top10.txt) is a 176-chunk run. On that run the direct targets ranked A: 2, B: 2, and D: 2; C's `laya_service.py` target remained outside the top 10. This is a partial improvement, not reliable retrieval across the four scenarios.

I then split syntax spans larger than 1,500 bytes into line-aligned chunks in `GitTrace::chunkify`. The index grew from 176 to 224 chunks. A fresh instructed hybrid top-10 run for C still returned unrelated historical `main.cc` query/usage code, the reranker HTTP client, and test code; `laya_service.py` did not appear. Thus, reducing the oversized Python function span alone did not fix this query. The result is a confirmed retrieval failure for C after the change.

The chunk-splitting code compiles, and the full `GitTrace` target built successfully. A clean-first build initially raced while regenerating vendored Metal assembly; after the generated `solve_tri` assembly had a duplicate boundary label removed, the full build completed. That generated file is under `build/`, not a source change. `git diff --check` passes. I did not run the automated test suite.

Next retrieval work should focus on evaluating each stage separately and auditing the query/corpus vectors and lexical ranks for the exact C target. Do not present these current A–D outputs as successful GitTrace search results in the survey; the C miss and other residual false positives need to be visible in the results.

The follow-up comparison now has explicit `vector`, `keyword`, and `hybrid` modes, plus basic token normalization. The same A–D queries were run at top 3 with the reranker disabled. Keyword-only found D's current fallback at rank 2; vector-only and hybrid found A's decay formula at rank 3; hybrid also found B's path-level ignore check at rank 3 and D's fallback at rank 3. None retrieved C in the top 3. Full results and command context are in [keyword-vector-hybrid-ablation.md](keyword-vector-hybrid-ablation.md).

## Post-fix rerun: path identity, score labels, and query expansion

On 2026-11-08, after the path-identity and score-label fixes, I reran all four queries against the full 224-chunk index with the one-shot CLI and reranking disabled. Complete outputs are linked in [the mode comparison](keyword-vector-hybrid-ablation.md) and [the query-expansion ablation](query-expansion-ablation.md). These replace the earlier hand-transcribed ranks for the same conditions.

Weighted expansion moved the lineage-decay target to rank 1 in both keyword and hybrid modes. It moved the path-level ignore check to rank 2 in keyword mode and rank 3 in hybrid. The Gemini candidate-safety target remained outside the top three in every tested mode. The unavailable-reranker fallback stayed rank 2 for keyword, rank 3 for unexpanded hybrid, and fell outside the top three for expanded hybrid. Therefore expansion stays opt-in; these four queries are diagnostic evidence, not a general quality claim.

The CLI now accepts repeated `--query` arguments, so a single indexing run can capture multiple queries without interactive input. Output distinguishes retrieval score, raw cosine, and lineage-adjusted similarity. This improves repeatability and score interpretation; it does not fix the C miss or establish calibrated relevance thresholds.
