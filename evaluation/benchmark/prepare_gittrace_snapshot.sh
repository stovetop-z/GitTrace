#!/usr/bin/env bash
set -euo pipefail

gittrace_source="${1:-$(pwd)}"
snapshot_root="${2:-/private/tmp/gittrace-benchmark/gittrace}"
source_revision="43a49a5ab34127612f7926a775636172467c401d"

if [[ -e "$snapshot_root" ]]; then
    echo "Snapshot destination already exists: $snapshot_root" >&2
    exit 1
fi

mkdir -p "$(dirname "$snapshot_root")"
git clone --quiet --shared --no-hardlinks "$gittrace_source" "$snapshot_root"
git -C "$snapshot_root" checkout --quiet "$source_revision"
cat > "$snapshot_root/.gittraceignore" <<'EOF'
docs/
evaluation/
llama.cpp/
build/
build-release/
README*
LICENSE
*.md
*.txt
*.json
*.jsonl
EOF

git -C "$snapshot_root" add -f .gittraceignore
git -C "$snapshot_root" config user.name "GitTrace Benchmark"
git -C "$snapshot_root" config user.email "benchmark@example.invalid"
git -C "$snapshot_root" commit -q -m "Set filtered benchmark corpus"

printf 'Snapshot: %s\n' "$snapshot_root"
printf 'GitTrace source revision: %s\n' "$source_revision"
printf 'Filtered snapshot revision: '
git -C "$snapshot_root" rev-parse HEAD
printf 'Commits indexed: '
git -C "$snapshot_root" rev-list --count HEAD
