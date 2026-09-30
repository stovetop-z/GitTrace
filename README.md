# GitTrace

GitTrace is a C++ prototype for finding related text with vector embeddings. It uses [llama.cpp](https://github.com/ggml-org/llama.cpp) to embed text and stores those embeddings in an in-memory `ChunkDB`. The current executable is a smoke test: it embeds three sample computer science descriptions, embeds a search query, and prints the closest samples by cosine similarity.

## Requirements

- CMake 3.10 or newer
- A C++17 compiler
- The [libgit2](https://libgit2.org/) development package
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

## Project layout

```text
main.cc                   Smoke-test executable
GitTrace/chunkdb/         In-memory embedding and similarity search
llama.cpp/                llama.cpp dependency
CMakeLists.txt            Build configuration
```

The project is under active development. `ChunkDB` currently stores chunks only in memory; it does not yet read Git history or persist an index between runs.
