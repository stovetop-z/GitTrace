# Weighted query-expansion ablation

Run date: 2026-11-08 (post-path-identity and score-label fixes)  
Repository HEAD: `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`  
Index: 35 unique text blobs, 27 parsed file versions, 224 chunks  
Reranker: disabled  
Query format: instructed for vector and hybrid  
Cutoff: top 3

The four queries are the same as in [keyword-vector-hybrid-ablation.md](keyword-vector-hybrid-ablation.md). The only changed variable in the expanded runs is the opt-in weighted synonym map (`--query-expansion`). Expansion terms have lower weights than the original query terms. The map covers general software-search relations such as `downweight → decay`, `older → historical`, `walk → tree/traversal`, `override → injection/untrusted`, and `rerank → reranker/relevance`.

Full outputs: [keyword with expansion](../post-fix-keyword-expanded-top3.txt) and [hybrid with expansion](../post-fix-hybrid-expanded-top3.txt). Compare these with the no-expansion files linked from the mode comparison.

## Target rank by retrieval mode

| Scenario | Keyword, no expansion | Vector | Hybrid, no expansion | Keyword + expansion | Hybrid + expansion |
|---|---:|---:|---:|---:|---:|
| A: lineage decay | >3 | 3 | 3 | **1** | **1** |
| B: history-walk ignore check | >3 | >3 | 3 | **2** | **3** |
| C: Gemini candidate safety instruction | >3 | >3 | >3 | >3 | >3 |
| D: unavailable-reranker fallback | 2 | >3 | 3 | **2** | >3 |

For B, the unexpanded lexical and hybrid results surfaced `.gittraceignore` loading, which is related but does not prove the check is applied to each path during the history walk. The expanded results included the direct `git_ignore_path_is_ignored` code at rank 2 for keyword-only and rank 3 for hybrid.

For D, keyword-only kept the current fallback code (`main.cc:163-182`) at rank 2 with or without expansion. Hybrid without expansion placed it at rank 3. Hybrid with expansion displaced it from the top 3, despite improving A and B. This appears to be a fusion interaction: the expanded terms add broad matches to generic CLI output and reranker-handling chunks.

## Result

The manual synonym map improves A in both lexical and hybrid modes, and improves B in keyword-only mode. Hybrid already retrieved B at rank 3 without expansion. Expansion does not recover C and hurts D in hybrid mode. Keep it opt-in while expanding the evaluation set. The map was designed from the A–D terminology, so these results show that it can address some of these vocabulary gaps; they do not establish general retrieval quality. Before enabling it by default, test unrelated repositories and queries, then consider reducing lexical expansion weight or using phrase-level intent detection to avoid broad matches such as `display → print/output`.

Reproduce the expanded hybrid run with:

```sh
./build/GitTrace --repo . --retrieval-mode hybrid --query-expansion --no-laya --top-k 3
```
