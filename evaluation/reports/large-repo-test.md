# Large repository indexing run

## Setup

- Repository: GitTrace itself, at `d77c3cc`
- Reachable commit history: 8 commits
- Run: `./build/GitTrace --repo . --no-interactive --profile evaluation/large-repo-cold-profile.json`
- Embedding backend: CPU
- SQLite cache: `.gittrace/index.sqlite` (1.9 MB)

This run indexed 35 unique file states from the repository history. Tree-sitter parsed 27; 8 were unsupported or invalid. GitTrace created 224 semantic chunks and made 224 embedding calls.

## Cold indexing timings

| Stage | Time |
| --- | ---: |
| Embedding model load | 4.77 s |
| History walk | 3.60 ms |
| Grammar preparation | 131.36 ms |
| Parsing | 14.17 ms |
| Chunking | 271.86 ms |
| Chunk embedding | 72 min 35.32 s |
| Lineage alignment | 35.87 ms |
| SQLite cache save | 8.17 ms |

The profiled stages total about **72 min 41 s**. Embedding ran on CPU and accounted for almost all of that time.

## Warm cache check

Immediately rerunning the same command loaded the index in 4.65 ms (manifest check: 4.95 ms), with `cache_hit=true`, zero embedding calls, and no history walk, model load, grammar preparation, parsing, or cache save. See [`large-repo-warm-profile.json`](../large-repo-warm-profile.json).

## Scope and limits

This is a full cold indexing run and a warm cache check, not a retrieval quality evaluation: the command used `--no-interactive`, so it made no search calls. The repository provides 224 chunks but only 8 reachable commits, so it is a useful indexing workload, not a representative deep-history benchmark. The 8 files that did not parse should be investigated before interpreting coverage as complete.

Machine-readable profiles: [`large-repo-cold-profile.json`](../large-repo-cold-profile.json) and [`large-repo-warm-profile.json`](../large-repo-warm-profile.json).
