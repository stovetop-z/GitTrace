# GitTrace

GitTrace is a C++ prototype for analyzing source files across a Git repository's history. It uses libgit2 to walk commits, collects unique text blob versions, parses supported files with Tree-sitter, and stores syntax-based chunks and embeddings in an in-memory `ChunkDB`.

## Requirements

- CMake 3.10 or newer
- A C++17 compiler
- The [libgit2](https://libgit2.org/) development package
- libcurl development files discoverable through `pkg-config`
- The Tree-sitter runtime development package and `pkg-config`
- CMake and a C/C++ compiler available on `PATH` (used to build downloaded grammars)
- Network access on the first run for each grammar used by the repository
- The `llama.cpp` source directory included in this repository
- The embedding model file described below
- SQLite 3 development library (discovered through `pkg-config` as `sqlite3`)

## Embedding model

`ChunkDB` loads the Jina v2 base code model from this path relative to the repository root:

```text
GitTrace/chunkdb/embedder/jina-embeddings-v2-base-code-Q4_K_M.gguf
```

Download `jina-embeddings-v2-base-code-Q4_K_M.gguf` from [gaianet/jina-embeddings-v2-base-code-GGUF](https://huggingface.co/gaianet/jina-embeddings-v2-base-code-GGUF) and place it at that path before running the program. It is a 109 MB Q4_K_M quantization with 768 dimensional output. Model files are ignored by Git, so each developer needs to provide the file locally. GitTrace uses the pooling mode recorded in each GGUF; changing models invalidates the cached index.

Choose another local GGUF with `--embedding-model PATH` or `GITTRACE_EMBEDDING_MODEL=PATH`. A model change selects a separate cache fingerprint automatically. Relative model paths are resolved from the current working directory.

## Build

From the repository root, configure and build with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
```

If CMake cannot find libgit2, configure with `-DLIBGIT2_ROOT=/path/to/libgit2-install`, where that prefix contains libgit2's CMake package files. The build does not assume a developer-specific installation path.

Use a Release build for normal indexing. On macOS, GitTrace can use Metal when the process has access to the system GPU. During a cold index, the CLI reports the selected backend and embedding progress; Debug builds can make CPU embedding substantially slower.

## Run GitTrace

Run from the repository root so the relative model and grammar paths resolve. With no argument, GitTrace scans the current repository; pass a path to scan another local repository:

```sh
./build/GitTrace
./build/GitTrace /path/to/repository
./build/GitTrace --repo /path/to/repository --top-k 8
```

After indexing, the CLI accepts natural-language or code queries. By default it combines vector and BM25 keyword rankings with reciprocal rank fusion, then posts the top candidates to the local reranking service for relevance decisions. Use `--retrieval-mode vector`, `keyword`, or `hybrid` to compare the retrieval lanes; `keyword` uses normalized BM25 terms without embedding the query. Keyword matching lowercases tokens, splits common identifier boundaries, and normalizes basic plural and verb endings. Add `--query-expansion` to include a small weighted synonym map in the lexical lane; it is off by default so the unexpanded baseline remains easy to reproduce. Results show the retrieval score, raw cosine, and lineage-adjusted similarity separately. The CLI displays only candidates accepted as `direct` or `related`; if the service is unavailable, it warns and displays results from the selected retrieval mode. Use `--no-laya` to bypass reranking or `--laya-url URL` to change the service endpoint. `GITTRACE_LAYA_URL` can also set the endpoint. `--no-interactive` ends after indexing.

For example, to inspect exact keyword matches without a reranker:

```sh
./build/GitTrace --repo /path/to/repository --retrieval-mode keyword --no-laya --top-k 10
```

For repeatable comparisons, pass one or more `--query` arguments. GitTrace indexes once, prints each query's ranked results, and exits:

```sh
./build/GitTrace --repo /path/to/repository --retrieval-mode hybrid \
  --no-laya --top-k 5 \
  --query "Where is the lineage decay applied?" \
  --query "How are ignored paths handled during the history walk?"
```

Queries use raw text by default with Jina. Pass `--query-format instructed` to compare against the earlier BGE-Code-v1 retrieval instruction wrapper; this is intended for retrieval evaluation. Corpus chunks remain embedded as source text in either mode.

At startup the CLI displays `Loading...` while initialization and indexing output is appended to `dev.log` in the current working directory. In interactive mode, successful startup proceeds directly to the `query>` prompt. Initialization and runtime errors are written to the same log.

Embedding-enabled runs save their chunks and embeddings in `.gittrace/index.sqlite` inside the scanned repository. An unchanged HEAD reuses the cached index. When HEAD advances from the cached commit, GitTrace walks only the new commits, adds their path/commit occurrences, and embeds only unseen file states. New chunks are embedded in bounded batches of up to four independent sequences per model call, subject to the model's token-batch limit. Causal models use `llama_decode`; encoder models use `llama_encode` with a microbatch large enough for the token batch. If any chunk fails to tokenize or embed, GitTrace reports the failures and refuses to save or use an incomplete index. Use `--index PATH` to choose another SQLite file and `--index-status` to inspect compatibility without loading the embedding model. The database contains source snippets and vectors, so keep it local and remove `.gittrace/` to delete the default cache. Use `--rebuild-index` to force a fresh scan and replace the cached index. `--profile PATH` writes JSON counts and stage timings for indexing, queries, and cache load/save; it distinguishes chunk inputs from model calls and includes embedding failures and the selected model path.

The cache fingerprint includes the repository `HEAD`, model path/size/modified time, platform, nested `.gitignore` files, `.git/info/exclude`, configured global excludes, grammar revisions and parser/scanner file metadata, Tree-sitter ABI, compiler version, chunker version, embedding input version, and embedding batch implementation. The batch implementation version invalidates indexes created before batching so a repository is rebuilt once instead of mixing vectors from separate and batched inference. Model and grammar source files are fingerprinted by metadata rather than full content hashes; if one is replaced while preserving both size and modification time, use `--rebuild-index`.

## Implementation and benchmark status (2026-11-25)

GitTrace now defaults to the smaller Jina code model. The CLI can select another local GGUF, cache compatibility follows the model path, and model-native pooling is used. Batched indexing handles both causal and encoder models. Tokenization or embedding failures are counted and prevent GitTrace from saving an incomplete index. CMake no longer assumes a developer-specific libgit2 installation path.

A partial C++ comparison used the same filtered GitTrace snapshot, scanner, raw queries, vector retrieval, and top-five output for both models. Each run indexed 417 chunks with zero embedding failures:

| Model | Embedding time | P@5 | Recall@5 | MRR |
|---|---:|---:|---:|---:|
| Jina Q4_K_M | 11.04 s | 0.089 | 0.222 | 0.222 |
| BGE Q8_0 | 99.41 s | 0.044 | 0.222 | 0.167 |

These nine-question results show Jina indexed this snapshot about nine times faster. The profiles report `automatic device selection` for both runs, but the actual GPU offload was not verified, so treat the speed difference as preliminary. See the [Jina profile](evaluation/benchmark/results/cpp-model-comparison/jina-gittrace-profile.json), [BGE profile](evaluation/benchmark/results/cpp-model-comparison/bge-gittrace-profile.json), [Jina log](evaluation/benchmark/results/cpp-model-comparison/jina-gittrace.log), and [BGE log](evaluation/benchmark/results/cpp-model-comparison/bge-gittrace.log).

The full two-repository comparison is unfinished. The BGE run on the curated `llama.cpp` snapshot was interrupted at 2,720 chunks; the matching Jina run was not started. No aggregate two-repository report was produced. The latest Release build succeeded. CTest passed 2/2 before the final encoder-dispatch change and still needs to be rerun. Next, rerun CTest, fix the comparison runner's per-run backend-log capture and interrupt cleanup, then finish both models on the same `llama.cpp` snapshot and verify actual device allocation.

## Git history grammar parsing

`GitTrace::beginGitTrace()` reads unique text blob versions from the repository history, collects their filename extensions, and looks up each extension in `GitTrace/rsc/grammar.txt`. For each configured grammar it clones the repository into `GitTrace/rsc/grammars/`, builds the generated parser sources as a shared library, loads the library, and parses the collected blobs. Grammar source checkouts and build products are local and ignored by Git. Subsequent runs reuse the source checkouts and rebuild the parser libraries; to refresh a grammar source checkout, remove its directory under `GitTrace/rsc/grammars/`.

This requires `tree-sitter` to be discoverable by `pkg-config` at configure time. On first use, CMake and a C/C++ toolchain must also be available on `PATH`, and the machine needs network access. Extensions absent from `grammar.txt` are skipped with a diagnostic. The manifest uses `extension = repository URL`, with optional `| source subdirectory | exported function` fields for repositories containing multiple parsers or keeping the parser below the repository root. When omitted, the source is read from the repository root and the exported function is inferred from the repository name (for example, `tree-sitter-cpp` maps to `tree_sitter_cpp`).

## Excluding files

Add a `.gittraceignore` file to the repository root to exclude paths from history collection, parsing, and search indexing. It accepts Git ignore syntax, including comments, glob patterns, directory patterns, and `!` negation rules. For example:

```gittraceignore
LICENSE
README*
docs/
!docs/architecture.md
```

Paths ignored by the repository's regular Git ignore files are also excluded.

## Laya relevance service

`laya_service.py` is a local HTTP sidecar for the C++ CLI. Start it before running an embedding-enabled GitTrace scan. GitTrace sends its vector-search candidates to `POST /rerank`; the selected backend returns a `direct`, `related`, or `irrelevant` decision, confidence, original similarity, and an `accepted` flag. It listens on `127.0.0.1:8787` by default. Laya is the default backend and loads its English checkpoint once when it starts. Laya judges relevance; neither backend proves that a chunk is factually correct.

The relevance rubric distinguishes evidence from nearby context and incidental keyword matches. `training/laya_relevance.jsonl` contains hand-labeled seed examples in JSON Lines format (`query`, `path`, `text`, `label`) for refining that rubric and building a future task-specific training or evaluation pipeline. These examples do not fine-tune the downloaded checkpoint by themselves; runtime behavior currently comes from the rubric in `laya_service.py`.

Install and start the service from the project root:

```sh
python3 -m venv .venv-laya
source .venv-laya/bin/activate
python -m pip install -r requirements-laya.txt
python laya_service.py
```

To use Gemini instead of running Laya locally, install the optional Gemini dependencies and select the backend. Set an API key in `GOOGLE_API_KEY` or `GEMINI_API_KEY`; `GITTRACE_GEMINI_MODEL` optionally selects another Gemini model (default `gemini-3.8-flash`). Candidate source snippets and queries are sent to Google's Gemini API, so use this option only when that data sharing is appropriate. API usage may incur charges.

```sh
python3 -m venv .venv-gemini
source .venv-gemini/bin/activate
python -m pip install -r requirements-gemini.txt
export GITTRACE_RERANKER=gemini
export GOOGLE_API_KEY="your-api-key"
python laya_service.py
```

Set `GITTRACE_RERANKER=laya` (the default) to use Laya instead. The C++ client and `/rerank` response format are the same for both backends. The Gemini backend groups candidates in small batches and requests structured JSON classifications.

Check readiness at `GET http://127.0.0.1:8787/health`. The C++ client can post candidate chunks like this:

```sh
curl -s http://127.0.0.1:8787/rerank \
  -H 'content-type: application/json' \
  -d '{
    "query": "Where is the embedding model initialized?",
    "min_relevance": "related",
    "candidates": [
      {
        "id": "candidate-1",
        "path": "GitTrace/chunkdb/chunkdb.cpp",
        "text": "bool ChunkDB::init() { ... }",
        "similarity": 0.62,
        "commit_sha": "abc123",
        "start_line": 15,
        "end_line": 28
      }
    ]
  }'
```

The response keeps each caller-provided `id`, so GitTrace can join the decisions back to its chunks. `min_relevance` accepts `direct`, `related` (the default), or `any`; every result is returned in relevance order with an `accepted` flag. Requests accept up to 64 candidates. The request's `max_len` defaults to 256 tokens to keep reranking responsive; the English checkpoint supports up to 512. Set `GITTRACE_LAYA_HOST` and `GITTRACE_LAYA_PORT` to change the bind address and port. Set `GITTRACE_LAYA_MODEL` or `GITTRACE_LAYA_SUBFOLDER` to select a different checkpoint.

## Project layout

```text
main.cc                   Git history scanner entry point
GitTrace/chunkdb/         Embeddings, retrieval, and SQLite index persistence
GitTrace/laya_client.*    HTTP client for the relevance service
GitTrace/utils/           Tree-sitter grammar loading and parsing
GitTrace/rsc/grammar.txt   File-extension to grammar manifest
laya_service.py           Local Laya or Gemini relevance reranker
llama.cpp/                llama.cpp dependency
CMakeLists.txt            Build configuration
```

The project is under active development. The current SQLite cache reuses a whole index for an unchanged manifest; incremental commit-by-commit refresh is not implemented yet.

Historical chunks are aligned heuristically within the same file. GitTrace compares chunks from older versions using embedding similarity, syntax-node type, and their relative order in the file. A matched chunk inherits a lineage depth; search similarity is then reduced by `exp(-0.12 × depth)` so newer versions rank ahead when otherwise equally relevant. This is a per-file snapshot heuristic: it does not currently use Git parent/merge relationships, and unmatched or low-confidence chunks start new lineages.
