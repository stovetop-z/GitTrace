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

## Embedding model

`ChunkDB` loads this model path relative to the repository root:

```text
GitTrace/chunkdb/embedder/bge-code-v1-q8_0.gguf
```

Place the compatible `bge-code-v1-q8_0.gguf` model at that path before running the program. Model files are ignored by Git, so each developer needs to provide the file locally.

## Build

From the repository root, configure and build with:

```sh
cmake -S . -B build -DLIBGIT2_ROOT=/path/to/libgit2-install
cmake --build build -j2
```

`LIBGIT2_ROOT` should point to the prefix containing libgit2's CMake package files. This project currently defaults it to `/Users/stovetop/lib/libgit2-install`; pass your own path when libgit2 is installed elsewhere.

## Run GitTrace

Run from the repository root so the relative model and grammar paths resolve. With no argument, GitTrace scans the current repository; pass a path to scan another local repository:

```sh
./build/GitTrace
./build/GitTrace /path/to/repository
./build/GitTrace --repo /path/to/repository --top-k 8
```

After indexing, the CLI accepts natural-language or code queries. It retrieves a wider vector-search candidate set, then posts those candidates to the local reranking service for relevance decisions. It displays only candidates accepted as `direct` or `related`; if the service is unavailable, it warns and displays the unreranked vector results. Use `--no-laya` to bypass reranking or `--laya-url URL` to change the service endpoint. `GITTRACE_LAYA_URL` can also set the endpoint. `--no-interactive` ends after indexing.

At startup the CLI displays `Loading...` while initialization and indexing output is appended to `dev.log` in the current working directory. In interactive mode, successful startup proceeds directly to the `query>` prompt. Initialization and runtime errors are written to the same log.

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
GitTrace/chunkdb/         In-memory embedding and similarity search
GitTrace/laya_client.*    HTTP client for the relevance service
GitTrace/utils/           Tree-sitter grammar loading and parsing
GitTrace/rsc/grammar.txt   File-extension to grammar manifest
laya_service.py           Local Laya or Gemini relevance reranker
llama.cpp/                llama.cpp dependency
CMakeLists.txt            Build configuration
```

The project is under active development. The in-memory index is rebuilt for each run and is not persisted between runs.

Historical chunks are aligned heuristically within the same file. GitTrace compares chunks from older versions using embedding similarity, syntax-node type, and their relative order in the file. A matched chunk inherits a lineage depth; search similarity is then reduced by `exp(-0.12 × depth)` so newer versions rank ahead when otherwise equally relevant. This is a per-file snapshot heuristic: it does not currently use Git parent/merge relationships, and unmatched or low-confidence chunks start new lineages.
