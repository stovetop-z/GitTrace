#!/usr/bin/env bash
set -euo pipefail

llama_source="${1:-$(pwd)/llama.cpp}"
snapshot_root="${2:-/private/tmp/gittrace-benchmark/llama.cpp}"
source_revision="a6ea155d3d38b3f6f43d0c0dc29c2d592413ef44"

if [[ -e "$snapshot_root" ]]; then
    echo "Snapshot destination already exists: $snapshot_root" >&2
    exit 1
fi

mkdir -p "$snapshot_root"
git -C "$llama_source" archive "$source_revision" \
    src/llama-context.cpp \
    src/llama-grammar.cpp \
    src/llama-kv-cache.cpp \
    src/llama-model-loader.cpp \
    src/llama-sampler.cpp \
    src/llama-vocab.cpp \
    src/llama-model.cpp \
    src/llama-arch.cpp \
    src/llama-batch.cpp \
    src/llama-mmap.cpp \
    common/chat.cpp \
    common/common.cpp \
    common/arg.cpp \
    tools/server/server.cpp \
    tools/server/server-context.cpp | tar -xf - -C "$snapshot_root"
cat > "$snapshot_root/.gittraceignore" <<'EOF'
docs/
tests/
models/
*.md
*.txt
*.csv
*.json
*.jsonl
*.yaml
*.yml
EOF

git -C "$snapshot_root" init -q
git -C "$snapshot_root" config user.name "GitTrace Benchmark"
git -C "$snapshot_root" config user.email "benchmark@example.invalid"
git -C "$snapshot_root" add -A
git -C "$snapshot_root" commit -q -m "Benchmark snapshot of llama.cpp source"

printf 'Snapshot: %s\n' "$snapshot_root"
printf 'Upstream revision: %s\n' "$source_revision"
printf 'Snapshot revision: '
git -C "$snapshot_root" rev-parse HEAD
