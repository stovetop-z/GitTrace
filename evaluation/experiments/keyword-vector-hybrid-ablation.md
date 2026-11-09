# Keyword, vector, and hybrid retrieval comparison

Run date: 2026-11-08 (post-path-identity and score-label fixes)  
Repository HEAD: `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`  
Index: 35 unique text blobs, 27 parsed file versions, 224 chunks  
Reranker: disabled (`--no-laya`)  
Query format: instructed for vector and hybrid; keyword mode does not embed the query  
Cutoff: top 3

All three modes were run against the same repository and these four scenario queries. These post-fix runs supersede earlier manual transcriptions; complete one-shot outputs are saved below.

| Mode | Captured output |
|---|---|
| Keyword, no expansion | [post-fix-keyword-top3.txt](../post-fix-keyword-top3.txt) |
| Vector | [post-fix-vector-top3.txt](../post-fix-vector-top3.txt) |
| Hybrid, no expansion | [post-fix-hybrid-top3.txt](../post-fix-hybrid-top3.txt) |

| Scenario | Query |
|---|---|
| A | What calculation downweights chunks from older revisions during search? |
| B | Are repository-local ignore rules applied to files throughout the commit history walk? |
| C | Can a retrieved code snippet override Gemini reranking instructions? |
| D | If the relevance reranker cannot be reached, what does the CLI display? |

## Direct-target results

| Scenario | Keyword only | Vector only | Hybrid |
|---|---|---|---|
| A: lineage decay | Not in top 3 | Rank 3: `GitTrace/chunkdb/chunkdb.cpp:230-246` | Rank 3: same target |
| B: ignore check during tree walk | No direct hit. Rank 1 only showed `.gittraceignore` loading in `GitTrace/gittrace.cpp:25-61` | No direct hit. Top results were historical walker code without the ignore check | Rank 3: `GitTrace/gittrace.cpp:115-148`, including `git_ignore_path_is_ignored` |
| C: Gemini candidate safety instruction | Not in top 3 | Not in top 3 | Not in top 3 |
| D: unavailable-reranker fallback | Rank 2: `main.cc:163-182` | Not in top 3 | Rank 3: `main.cc:163-182` |

## What the keyword lane returned

- **A:** current and historical `GitTrace::searchChunks` wrappers, followed by `test.cpp`; none contains the decay formula.
- **B:** current initialization code that loads `.gittraceignore`, test code, then the current history-walk chunk containing the direct path ignore check at rank 3.
- **C:** interactive query-prompt code, then lineage and reranker-client code. It did not retrieve `laya_service.py`'s Gemini instruction.
- **D:** an older reranker-result handling block, the current fallback block at rank 2, and an older copy of that block.

Keyword search is useful when the query shares distinctive terms with the target, as in D. It cannot bridge vocabulary gaps by itself: A describes `lineage_depth` as “older revisions,” and C describes prompt-injection safety without using the target's core word `untrusted`. Hybrid improves B to rank 3 because the vector lane adds the direct chunk even though keyword-only does not find it in the top 3.

## Implementation

`--retrieval-mode vector|keyword|hybrid` now isolates the ranking lanes. The keyword mode uses BM25 over chunk text and file paths, with lowercase tokens, basic identifier-boundary splitting, and conservative normalization for common plural and verb endings. Hybrid mode retains reciprocal-rank fusion. All modes can be inspected without a reranker using `--no-laya`.

Example:

```sh
./build/GitTrace --repo . --retrieval-mode keyword --no-laya --top-k 10
```

This is a small four-query diagnostic, not a general effectiveness estimate. The current keyword normalization did not materially improve A–C. The next experiment should add a controlled synonym/identifier expansion lane (for example, mapping “older revisions” to `lineage_depth`, and “instruction injection” to `untrusted`) and compare it on the same gold chunks. Keep that separate from keyword-only scores so the source of any improvement remains measurable.
