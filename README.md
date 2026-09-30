# GitTrace

GitTrace is a C++ prototype for finding related text with vector embeddings. It uses [llama.cpp](https://github.com/ggml-org/llama.cpp) to embed text and stores those embeddings in an in-memory `ChunkDB`. The current executable is a smoke test: it embeds three sample computer science descriptions, embeds a search query, and prints the closest samples by cosine similarity.

## Requirements

- CMake 3.10 or newer
- A C++17 compiler
- The [libgit2](https://libgit2.org/) development package
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

## Run the smoke test

Run from the repository root so the relative model path resolves:

```sh
./build/GitTrace
```

The program prints whether initialization and each embedding succeeded, followed by up to three matching chunks. It exits with a nonzero status if initialization or embedding fails.

## Git history grammar parsing

`GitTrace::beginGitTrace()` reads unique text blob versions from the repository history, collects their filename extensions, and looks up each extension in `GitTrace/rsc/grammar.txt`. For each configured grammar it clones the repository into `GitTrace/rsc/grammars/`, builds the generated parser sources as a shared library, loads the library, and parses the collected blobs. Grammar source checkouts and build products are local and ignored by Git. Subsequent runs reuse those checkouts and libraries; to refresh a grammar, remove its directory under `GitTrace/rsc/grammars/`.

This requires `tree-sitter` to be discoverable by `pkg-config` at configure time. On first use, CMake and a C/C++ toolchain must also be available on `PATH`, and the machine needs network access. Extensions absent from `grammar.txt` are skipped with a diagnostic. The manifest uses `extension = repository URL`, with optional `| source subdirectory | exported function` fields for repositories containing multiple parsers or keeping the parser below the repository root. When omitted, the source is read from the repository root and the exported function is inferred from the repository name (for example, `tree-sitter-cpp` maps to `tree_sitter_cpp`).

## Project layout

```text
main.cc                   Smoke-test executable
GitTrace/chunkdb/         In-memory embedding and similarity search
llama.cpp/                llama.cpp dependency
CMakeLists.txt            Build configuration
```

The project is under active development. `ChunkDB` currently stores chunks only in memory; it does not yet read Git history or persist an index between runs.
