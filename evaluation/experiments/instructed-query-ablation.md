# BGE-Code-v1 query-format ablation

Date: 2026-11-06  
Model: `bge-code-v1-q8_0.gguf` (1536 dimensions; llama.cpp reports last-token pooling)  
Fixture commit: `d8d37cbda4b59cd1d6ec0c419a37ae3dd83cf24c` in `/private/tmp/gittrace-retrieval-fixture`  
Corpus: one commit, five file versions, 10 semantic chunks  
Mode: vector search, `--no-laya`, top 3

Each query was run against the same small fixture in raw-query mode and instructed-query mode. Corpus embeddings were unchanged. In instructed mode, GitTrace wraps only the query as `<instruct>{task}\n<query>{query}`. This follows the query/corpus distinction in [BAAI's BGE-Code-v1 model card](https://huggingface.co/BAAI/bge-code-v1/blob/main/README.md).

| Scenario | Raw query top 1 | Instructed query top 1 | Gold target rank |
|---|---|---|---|
| A | `src/scoring.cpp:7` (`ChunkMatch` declaration), `0.4909` | `src/scoring.cpp:9-25` (`ChunkDB::search` with exponential decay), `0.3630` | Raw: outside top 3; instructed: 1 |
| B | `src/history_ignore.cpp:19-26` (historical path skip), `0.4577` | Same target, `0.4068` | 1 in both |
| C | `src/rerank_prompt.py:1-8` (untrusted candidate instruction), `0.3919` | Same target, `0.3013` | 1 in both |
| D | `src/cli_fallback.cpp:6-17` (warn and print vector matches), `0.4470` | Same target, `0.4125` | 1 in both |

On this fixture, the instruction changed A from a top-three miss to rank 1 and preserved the rank-1 target for B–D. Scores decreased when instructed, which confirms that these are relative cosine values rather than relevance probabilities. The raw top-three lists for B–D included some unrelated results behind the target; the instructed lists did too.

This is a positive but narrow result: the fixture contains the answer code plus a few deliberately selected distractors and has only 10 chunks. It does not establish that the full GitTrace repository will retrieve the four targets correctly. The full-repository instructed run was canceled during indexing before it returned results. The next validation is to add realistic distractors and current/history duplicates, then measure target rank on the full repository once indexing completes.

Reproduction commands, from the GitTrace repository root:

```sh
printf "%s\\n" \
  "What calculation downweights chunks from older revisions during search?" \
  "Are repository-local ignore rules applied to files throughout the commit history walk?" \
  "Can a retrieved code snippet override Gemini reranking instructions?" \
  "If the relevance reranker cannot be reached, what does the CLI display?" \
  ":q" | ./build/GitTrace --repo /private/tmp/gittrace-retrieval-fixture --top-k 3 --no-laya --query-format raw
```

Repeat with `--query-format instructed` to compare. The fixture repository is temporary and is not part of the product corpus.
