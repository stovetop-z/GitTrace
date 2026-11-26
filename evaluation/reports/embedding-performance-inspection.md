# Embedding performance inspection

## Summary

The cold index took about **72 min 41 s** in the profiled run. This was not Git history traversal, parsing, or SQLite work: GitTrace spent **72 min 35 s** in chunk embedding. The observed run combined three costly conditions:

1. The 1.5B parameter BGE Code v1 model ran on CPU because the process could not obtain a Metal device.
2. GitTrace makes a separate, sequential `llama_decode` call for every chunk.
3. The current build is `Debug`; its compile commands include `-g` and no optimization flag such as `-O2` or `-O3`.

The run processed 224 chunks, averaging **19.4 seconds per embedding call**. Parsing 35 file versions took 14 ms, chunking took 272 ms, model initialization took 4.77 s, and SQLite save took 8.17 ms. The resulting 1.9 MB SQLite cache is reusable: an unchanged second run reported a cache hit and loaded it in 4.65 ms without embedding.

Follow-up testing isolated the execution-environment issue: the sandbox cannot access Metal, but the host can. A Release build in the host context offloaded all model layers and completed the same 224-chunk cold index in **47.5 s of embedding time** (about 56 s across profiled stages). The README now recommends Release for normal builds.

## Follow-up: smaller embedding model

GitTrace now defaults to `jina-embeddings-v2-base-code` in Q4_K_M GGUF form, with mean pooling. A PyTorch MPS run encoded 1,135 Python chunks at 16.22 chunks/second. On 18 labeled questions, vector-only retrieval scored P@5 0.233, Recall@5 0.722, and MRR 0.415. A one-chunk C++ smoke run successfully indexed and retrieved the expected snippet, but used CPU fallback and is not a throughput measurement. These results support trying the smaller model as the default, but do not establish a speed or quality win over BGE: BGE's PyTorch runs were incomplete, the Python and C++ chunkers differ, and the benchmark is small. See [`../benchmark/pytorch-index-results.md`](../benchmark/pytorch-index-results.md) for data and limits.

## Evidence

### Cold run profile

The measurements come from [`large-repo-cold-profile.json`](../large-repo-cold-profile.json) and [`large-repo-test.md`](large-repo-test.md):

- `chunks_created`: 224
- `embedding_calls`: 224
- `chunk_embedding_ms`: 4,355,320 ms (72 min 35.32 s)
- `embedding_model_load_ms`: 4,770.82 ms
- `parsing_ms`: 14.17 ms
- `cache_save_ms`: 8.17 ms

The matching warm profile is [`large-repo-warm-profile.json`](../large-repo-warm-profile.json): `cache_hit=true`, `embedding_calls=0`, `cache_load_ms=4.653`, and `manifest_ms=4.945`.

### Model and execution path

- [`chunkdb.h`](../../GitTrace/chunkdb/chunkdb.h) selects `bge-code-v1-q8_0.gguf`. The file is 1.5 GB; its GGUF metadata identifies it as **BGE Code v1, 1.5B parameters**, Q8 quantized, with 28 Qwen2 blocks and 1,536 dimensional embeddings.
- [`chunkdb.cpp`](../../GitTrace/chunkdb/chunkdb.cpp) loads that model once, then `ChunkDB::embed` tokenizes each input, clears the model context, creates a one-input batch with `llama_batch_get_one`, calls `llama_decode`, and extracts one embedding.
- [`gittrace.cpp`](../../GitTrace/gittrace.cpp) calls `ChunkDB::embed` inside the chunk loop. Each chunk is processed serially. `metrics.embedding_calls` increments once for each call, and the profile confirms 224 calls for 224 chunks.
- The code sets `n_ctx=2048`. The vendored llama.cpp default is four threads (`GGML_DEFAULT_N_THREADS=4`); GitTrace does not override `n_threads` or `n_threads_batch`.

### Metal availability

The build is not missing Metal support:

- `build/CMakeCache.txt` has `GGML_METAL=ON`, `GGML_ACCELERATE=ON`, and Metal backend dylibs are present under `build/bin`.
- `system_profiler SPDisplaysDataType` reports an Apple M3 GPU with 10 cores and “Metal: Supported.”
- The cold-run llama.cpp log reports `picking default device: (null)`, followed by failure to create a command queue and failure to allocate the Metal backend context. GitTrace then reports that it is retrying CPU-only and successfully loads the CPU backend.
- A minimal Objective-C Metal probe launched in the sandbox got `MTLCreateSystemDefaultDevice returned nil`. Running the same probe in the host context returned `device=Apple M3` and created a command queue. This confirms Metal access was restricted by the sandbox used for the initial long run.
- Running the Release GitTrace binary in the host context found `Apple M3` and offloaded **29/29 model layers to GPU**. The run completed all 224 embeddings. It had a cache miss because `.gitignore` had changed to ignore `build-release/`, changing the indexer's ignore-rule fingerprint; it rebuilt and saved the index correctly.

### Build optimization

- `build/CMakeCache.txt` says `CMAKE_BUILD_TYPE=Debug`.
- `build/compile_commands.json` shows the llama.cpp CPU kernels and GitTrace code compiled with `-g` and without `-O` optimization flags.
- The CPU backend is built with native ARM CPU tuning and Accelerate support, so the build is not wholly generic. The repeated-query comparison below measured a substantial Debug penalty, although it does not isolate the full-corpus indexing speedup.
- `GGML_OPENMP_ENABLED` is `OFF`. llama.cpp still has its own CPU thread pool, so this alone does not prove that inference is single-threaded; the default thread count is four. Thread count should be benchmarked instead of assumed to be the main cause.

## Findings: confirmed versus unresolved

**Confirmed:** the initial run used CPU fallback inside the sandbox, the model is 1.5B parameters, calls are sequential per chunk, and the Debug build adds a large CPU penalty. The host context can access Metal; the Release app offloaded all model layers and completed this indexing run in under a minute.

### Release versus Debug query benchmark

A separate optimized build was configured at `build-release` and run against the same cached 224-chunk index. Five identical queries were executed per process. Both runs used CPU fallback inside the sandbox.

| Build | First query (includes lazy model initialization) | Next four queries, average |
| --- | ---: | ---: |
| Debug | 10.15 s | 4.27 s |
| Release | 3.73 s | 0.21 s |

The warm repeated query was about **20x faster** in Release in this sample. This is strong evidence that the unoptimized build contributes heavily to CPU latency. It is still a short natural-language query benchmark over 224 cached chunks, not a controlled corpus-chunk embedding benchmark; do not apply the 20x factor directly to the 72-minute cold indexing time. Profiles: [`debug-query-repeat5-profile.json`](../debug-query-repeat5-profile.json) and [`release-query-repeat5-profile.json`](../release-query-repeat5-profile.json). The Release configure output also confirms OpenMP is unavailable in this toolchain, while llama.cpp's built-in four-thread CPU pool remains configured.

### Progress and backend reporting

The CLI now displays embedding progress every 10 calls, with completed chunks, elapsed time, and rolling chunks per second. Profiles record `embedding_backend` and `embedding_threads`, and the CLI reports these after a run or query. An 11-chunk fixture showed progress at 10 and 11 chunks; its profile recorded `CPU fallback` and four threads. The host query check recorded `automatic device selection`, four threads, and logs confirmed 29/29 GPU layers. See [`progress-smoke-profile2.json`](../progress-smoke-profile2.json) and [`metal-host-profile-after-observability.json`](../metal-host-profile-after-observability.json).

### Release indexing with host Metal

The Release binary was run in the host context after confirming Metal access. GitTrace offloaded 29/29 layers and indexed the same 224 chunks:

- `embedding_model_load_ms`: 6,662.64 ms
- `chunk_embedding_ms`: 47,477.5 ms (47.48 s)
- `embedding_calls`: 224
- `cache_save_ms`: 11.09 ms
- first query in the same process: 84.50 ms

The profiled stage total was about **56.1 s**, compared with about **72 min 41 s** for the earlier Debug CPU run. That is roughly **92x faster end to end**, combining the optimized build and GPU access; the two factors have not been separated on full corpus chunks. A later warm-cache run from the sandbox reported `cache_hit=true` and a 3.22 ms load. Profiles: [`metal-host-query-profile.json`](../metal-host-query-profile.json) and [`metal-host-warm-profile.json`](../metal-host-warm-profile.json).

**Still worth measuring:** how much of the full-corpus speedup came from Release versus GPU; whether batching or a smaller model improves on a 47-second GPU indexing baseline; and the average token count per chunk. The immediate operational fix is to use a Release binary outside the GPU-restricted sandbox.

The host run executed one retrieval query after indexing, but this was not a retrieval quality evaluation. The repository had only 8 reachable commits, although its history produced 35 file versions and 224 chunks.

## Recommended next steps

1. **Use the right runtime/build.** README build instructions now configure Release. Run GitTrace from a host terminal when GPU access is available; the sandbox used for the slow run cannot see Metal, while the host can.
2. **Measure the bounded batching implementation.** GitTrace sends up to four independent chunk sequences per model call, constrained by the token batch. Decoder models use `llama_decode`; encoder models use `llama_encode` and set `n_ubatch` to the configured token batch. Profiles distinguish `embedding_inputs` from `embedding_model_calls` and include failed inputs.
3. **Make cold builds resumable for much larger repositories.** Checkpointing is less urgent for this workload but remains useful if real repositories take many minutes.
4. **Compare the new default against the old model under controlled conditions.** Index the same fixed corpus with BGE and Jina using the same C++ build, backend, chunk boundaries, and batch settings; then run the same labeled queries and report throughput, P@5, Recall@5, and MRR. Keep the current Jina results labeled exploratory until that comparison is complete.

The progress/backend changes built in both Debug and Release. Release CTest passed 2/2 tests. The 11-chunk progress smoke run visibly reported progress and completed with the expected CPU fallback/thread metadata.

The existing cache prevents this cost from recurring for an unchanged repository and manifest. New file states still require embeddings, while unchanged states can reuse the persisted vectors.
