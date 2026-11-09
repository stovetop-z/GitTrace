# A–D retrieval comparison after the performance fix

Run date: 2026-11-09  
Repository HEAD: `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`  
Index: 224 chunks, cache hit in all modes  
Binary: Release build, run in host context with Metal available for vector and hybrid  
Reranker: disabled (`--no-laya`)  
Query expansion: off  
Result depth: top 5

Each mode used the same A–D wording from [`scenario-questions.txt`](../scenarios/scenario-questions.txt). Complete CLI output and machine-readable timing profiles are saved per mode:

| Mode | Full top-five output | Profile |
|---|---|---|
| Vector | [`post-performance-vector-top5.txt`](../post-performance-vector-top5.txt) | [`post-performance-vector-profile.json`](../post-performance-vector-profile.json) |
| Keyword | [`post-performance-keyword-top5.txt`](../post-performance-keyword-top5.txt) | [`post-performance-keyword-profile.json`](../post-performance-keyword-profile.json) |
| Hybrid | [`post-performance-hybrid-top5.txt`](../post-performance-hybrid-top5.txt) | [`post-performance-hybrid-profile.json`](../post-performance-hybrid-profile.json) |

## Direct-target rank

| Scenario | Intended evidence | Vector | Keyword | Hybrid |
|---|---|---:|---:|---:|
| A | `chunkdb.cpp`: cosine similarity multiplied by `exp(-0.12 * lineage_depth)` | **3** | Outside top 5 | **3** |
| B | `gittrace.cpp`: checks each historical path with `git_ignore_path_is_ignored` | Outside top 5 | **4** | **4** |
| C | `laya_service.py`: retrieved candidate text is untrusted, not instructions | Outside top 5 | Outside top 5 | Outside top 5 |
| D | `main.cc`: warns when reranker is unavailable, then prints vector matches | Outside top 5 | **2** | **3** |

Target hits were checked against both the file and commit. A and D have many historical versions, and several high-ranked results are old code. For B, vector returned old history-walk chunks from commits `9d755e0…` and `e2db25d…`; those do not show the current repository-local ignore handling. The current B evidence is `GitTrace/gittrace.cpp:115-148` at commit `d77c3c…`, ranked fourth by keyword and hybrid. The C target did not appear in any top-five list.

## Timing

All three runs were cache hits and searched the same 224 chunks. The first vector and hybrid query includes lazy model/backend initialization, so the later-query timings are more representative of repeated use.

| Mode | Four-query total | First query | Mean of next three |
|---|---:|---:|---:|
| Vector | 3.78 s | 3.51 s | 88.6 ms |
| Keyword | 7.2 ms | 1.9 ms | 1.8 ms |
| Hybrid | 3.17 s | 2.91 s | 84.8 ms |

Vector and hybrid profiles report automatic device selection with four threads; the host run's Metal log confirms 29/29 layers were GPU-offloaded. Keyword mode does not initialize the embedding model.

## Assessment

Hybrid is the strongest of these three on this small diagnostic: it retrieves A, B, and D within five results. Keyword is very fast and finds B and D, but not A. Vector finds A while returning irrelevant or historical candidates for B–D. C remains a consistent miss and should stay visible in any survey or product claim.

These four queries are a diagnostic set, not a broad quality estimate. Scores are ranking values, not calibrated probabilities, and GitTrace still returns five results even if none is relevant. Keep the commit hash visible when judging historical results.

## Why Scenario C misses

The active root `.gittraceignore` contains `*.py` on line 4. GitTrace loads this local file and checks every historical tree path against the ignore rules before parsing and chunking it. `laya_service.py` is tracked in Git, but the current SQLite index has **zero Python chunks** and zero rows for `laya_service.py` in both `chunks` and `occurrences`. Therefore the Gemini safety instruction at `laya_service.py:139` is absent from the searchable corpus; this miss is caused by corpus filtering, not by vector ranking.

`.gittraceignore` itself is ignored by `.gitignore`, so this is a local index setting rather than a committed repository rule. The smallest change to test is to narrow `*.py` or add an exception for `laya_service.py`, then rebuild the index. The ignore-file contents are part of the index fingerprint, so changing the rule correctly forces a fresh index. Do not compare C retrieval before and after that reindex as though only ranking changed: the corpus changed too.

## Scenario C rerun with the file included

Added `!laya_service.py` immediately after `*.py` in the local `.gittraceignore`. A no-embedding scan confirmed both historical versions are now considered. Rebuilding with the Release binary and host Metal produced 239 chunks from 37 unique file states (up from 224 chunks and 35 states); `laya_service.py` contributes 15 chunks and 30 occurrence rows. Embedding took 51.93 s across 239 calls.

The exact C query was rerun with top 5 and no reranker:

| Mode | Rank of current `laya_service.py` instruction chunk | Query time |
|---|---:|---:|
| Vector | **1** | 974 ms |
| Keyword | **3** | 2.5 ms |
| Hybrid | **1** | 1.55 s |

All hits are from current commit `d77c3ccebcd22970ed4b21c58d35a810b60fa72b`; the rank-1 snippet includes the system instruction to treat candidate text as untrusted. This confirms the previous C miss came from filtering, not ranking. Full results and profiles: [`scenario-c-exception-vector-top5.txt`](../scenario-c-exception-vector-top5.txt), [`scenario-c-exception-keyword-top5.txt`](../scenario-c-exception-keyword-top5.txt), [`scenario-c-exception-hybrid-top5.txt`](../scenario-c-exception-hybrid-top5.txt), and the matching `scenario-c-exception-*-profile.json` files.
