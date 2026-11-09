# SQLite cache smoke test

**Date:** 2026-11-08  
**Fixture:** temporary repository, one C++ file, one commit, one semantic chunk  
**Platform:** macOS arm64; llama.cpp selected CPU fallback

The CLI was run twice with `--no-interactive --profile PATH` against the same fixture. The cold run scanned, parsed, embedded, and saved one chunk. The warm run loaded the chunk from SQLite and skipped history traversal, grammar preparation, parsing, corpus embedding, and model initialization.

| Stage | Cold | Warm |
|---|---:|---:|
| Repository initialization | 0.21 ms | 1.25 ms |
| Embedding model load | 6,650 ms | 0 ms |
| Git history walk | 6.28 ms | 0 ms |
| Grammar preparation | 353 ms | 0 ms |
| Parsing | 3.24 ms | 0 ms |
| Chunk construction | 18.36 ms | 0 ms |
| Chunk embedding | 1,735 ms | 0 ms |
| Lineage alignment | 0.007 ms | 0 ms |
| SQLite cache load | 0.007 ms (miss check) | 0.90 ms |
| SQLite cache save | 9.31 ms | 0 ms |

Machine-readable values are in [`cache-smoke-results.json`](../cache-smoke-results.json).

The warm profile had `cache_hit=true`, one cached chunk/file version, and zero commits visited, parser calls, embedding calls, or model loads. `--index-status` also reported the custom index as compatible without initializing the model.

This proves the warm cache path works for the tested fixture; it is not a representative performance benchmark. In an interactive vector/hybrid run, the embedding model is loaded lazily when the first query is submitted, so the prompt appears quickly but the first vector query still pays model-load latency. A meaningful performance claim requires cold/warm runs on larger repositories and explicit first-query timing.
