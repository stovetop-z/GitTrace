# GitTrace

GitTrace is a C++ prototype for analyzing source files across a Git repository's history. It uses libgit2 to walk commits, collects unique text blob versions, and uses Tree-sitter to parse files whose extensions have configured grammars. `ChunkDB` initializes an embedding model, but the current history scan does not yet store parsed code or embeddings in the database.

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

## Run GitTrace

Run from the repository root so the relative model and grammar paths resolve. With no argument, GitTrace scans the current repository; pass a path to scan another local repository:

```sh
./build/GitTrace
./build/GitTrace /path/to/repository
```

The program reports initialization, grammar setup and parsing results, and the number of unique text blob versions collected. It exits with a nonzero status if initialization fails.

## Git history grammar parsing

`GitTrace::beginGitTrace()` reads unique text blob versions from the repository history, collects their filename extensions, and looks up each extension in `GitTrace/rsc/grammar.txt`. For each configured grammar it clones the repository into `GitTrace/rsc/grammars/`, builds the generated parser sources as a shared library, loads the library, and parses the collected blobs. Grammar source checkouts and build products are local and ignored by Git. Subsequent runs reuse the source checkouts and rebuild the parser libraries; to refresh a grammar source checkout, remove its directory under `GitTrace/rsc/grammars/`.

This requires `tree-sitter` to be discoverable by `pkg-config` at configure time. On first use, CMake and a C/C++ toolchain must also be available on `PATH`, and the machine needs network access. Extensions absent from `grammar.txt` are skipped with a diagnostic. The manifest uses `extension = repository URL`, with optional `| source subdirectory | exported function` fields for repositories containing multiple parsers or keeping the parser below the repository root. When omitted, the source is read from the repository root and the exported function is inferred from the repository name (for example, `tree-sitter-cpp` maps to `tree_sitter_cpp`).

## Project layout

```text
main.cc                   Git history scanner entry point
GitTrace/chunkdb/         In-memory embedding and similarity search
GitTrace/utils/           Tree-sitter grammar loading and parsing
GitTrace/rsc/grammar.txt   File-extension to grammar manifest
llama.cpp/                llama.cpp dependency
CMakeLists.txt            Build configuration
```

The project is under active development. GitTrace currently reports parsing results but does not yet extract syntax nodes, create embeddings for historical files, or persist an index between runs.
