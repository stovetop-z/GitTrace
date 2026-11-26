#!/usr/bin/env python3
"""Compare two GGUF embedding models in GitTrace on identical C++ inputs."""

from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
import time
from datetime import date
from pathlib import Path

from run_benchmark import aggregate, load_questions, parse_outputs, score_question

ROOT = Path(__file__).resolve().parents[2]


def run_condition(binary: Path, repo: Path, repo_name: str, questions: list[dict],
                  snapshot_sha: str, model_path: Path, model_name: str,
                  index_path: Path, output_dir: Path) -> dict:
    stem = f"{model_name}-{repo_name}"
    log_path = output_dir / f"{stem}.log"
    profile_path = output_dir / f"{stem}-profile.json"
    dev_log_path = output_dir / f"{stem}-dev.log"
    command = [
        str(binary), "--repo", str(repo), "--embedding-model", str(model_path),
        "--retrieval-mode", "vector", "--query-format", "raw", "--top-k", "5",
        "--no-interactive", "--no-laya", "--rebuild-index",
        "--index", str(index_path), "--profile", str(profile_path),
    ]
    for question in questions:
        command.extend(["--query", question["question"]])

    start = time.perf_counter()
    process = subprocess.Popen(
        command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, bufsize=1,
    )
    output: list[str] = []
    assert process.stdout is not None
    with log_path.open("w") as stream:
        for line in process.stdout:
            stream.write(line)
            output.append(line)
            message = line.strip()
            if message.startswith(("Embedding:", "Embedding complete:", "Query:", "Embedding backend:")):
                print(f"    {message}", flush=True)
    code = process.wait()
    wall_seconds = time.perf_counter() - start
    if code:
        raise RuntimeError(f"{stem} failed with exit code {code}; see {log_path}")

    active_dev_log = ROOT / "dev.log"
    if active_dev_log.exists():
        shutil.copyfile(active_dev_log, dev_log_path)
    profile = json.loads(profile_path.read_text())
    if profile.get("embedding_failures", 0):
        raise RuntimeError(f"{stem} reported embedding failures; see {log_path}")
    profile["repository"] = repo_name
    profile["embedding_model_path"] = model_path.name
    profile_path.write_text(json.dumps(profile, indent=2) + "\n")
    parsed = parse_outputs("".join(output), questions)
    rows = [score_question(q, parsed[q["id"]], snapshot_sha) for q in questions]
    dev_log = dev_log_path.read_text(errors="replace") if dev_log_path.exists() else ""
    offloads = re.findall(r"offloaded\s+(\d+)/(\d+) layers to GPU", dev_log)
    gpu_offload_fraction = None
    if offloads:
        offloaded, total = map(int, offloads[-1])
        gpu_offload_fraction = offloaded / total if total else None
    return {
        "model_path": model_path.name,
        "repo": repo_name,
        "snapshot_sha": snapshot_sha,
        "backend": profile.get("embedding_backend"),
        "gpu_offload_fraction": gpu_offload_fraction,
        "profile": profile,
        "wall_seconds": wall_seconds,
        "log": log_path.name,
        "dev_log": dev_log_path.name if dev_log_path.exists() else None,
        "questions": rows,
        "metrics": aggregate(rows),
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--gittrace-snapshot", type=Path, required=True)
    parser.add_argument("--llama-snapshot", type=Path, required=True)
    parser.add_argument("--jina-model", type=Path, required=True)
    parser.add_argument("--bge-model", type=Path, required=True)
    parser.add_argument("--index-dir", type=Path, required=True,
                        help="Temporary directory for model-specific SQLite indexes")
    parser.add_argument("--output-dir", type=Path,
                        default=Path(__file__).with_name("results") / "cpp-model-comparison")
    args = parser.parse_args()

    binary = args.binary.resolve()
    repos = {
        "gittrace": args.gittrace_snapshot.resolve(),
        "llama_cpp": args.llama_snapshot.resolve(),
    }
    models = {"jina": args.jina_model.resolve(), "bge": args.bge_model.resolve()}
    for path in [binary, *repos.values(), *models.values()]:
        if not path.exists():
            parser.error(f"required path does not exist: {path}")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    args.index_dir.mkdir(parents=True, exist_ok=True)
    questions = load_questions()
    runs: dict[str, dict] = {}
    for model_name in models:
        runs[model_name] = {}
    for repo_name, repo_path in repos.items():
        repo_questions = [q for q in questions if q["repo"] == repo_name]
        snapshot_sha = subprocess.check_output(
            ["git", "-C", str(repo_path), "rev-parse", "HEAD"], text=True
        ).strip()
        model_order = ("jina", "bge") if repo_name == "gittrace" else ("bge", "jina")
        for model_name in model_order:
            model_path = models[model_name]
            print(f"\n{model_name}/{repo_name} ({len(repo_questions)} questions, {snapshot_sha[:12]})",
                  flush=True)
            index_path = args.index_dir / f"{model_name}-{repo_name}.sqlite"
            result = run_condition(
                binary, repo_path, repo_name, repo_questions, snapshot_sha,
                model_path, model_name, index_path, args.output_dir,
            )
            runs[model_name][repo_name] = result

        jina_chunks = runs["jina"][repo_name]["profile"]["chunks_created"]
        bge_chunks = runs["bge"][repo_name]["profile"]["chunks_created"]
        if jina_chunks != bge_chunks:
            raise RuntimeError(
                f"Chunk counts differ for {repo_name}: Jina={jina_chunks}, BGE={bge_chunks}"
            )

    result_data = {
        "run_date": date.today().isoformat(),
        "binary": binary.name,
        "query_format": "raw",
        "retrieval_mode": "vector",
        "reranking": False,
        "models": runs,
    }
    json_path = args.output_dir / "metrics.json"
    json_path.write_text(json.dumps(result_data, indent=2) + "\n")

    overall = {}
    for model_name, by_repo in runs.items():
        rows = [row for repo_result in by_repo.values() for row in repo_result["questions"]]
        overall[model_name] = aggregate(rows)
    backends = {
        result["backend"]
        for by_repo in runs.values() for result in by_repo.values()
    }
    offload_modes = {
        result["gpu_offload_fraction"]
        for by_repo in runs.values() for result in by_repo.values()
    }
    if len(backends) != 1 or len(offload_modes) != 1 or None in offload_modes:
        raise RuntimeError(
            "The comparison did not use one verified embedding backend/offload mode: "
            f"backends={sorted(backends)}, GPU offload fractions={sorted(str(x) for x in offload_modes)}"
        )
    lines = [
        "# Controlled C++ embedding model comparison", "",
        f"Run date: {result_data['run_date']}",
        f"GitTrace binary: `{binary}`",
        "Conditions: same C++ chunker and scanner, raw queries, vector retrieval, top 5, no reranker.",
        f"Backends observed: {', '.join(sorted(backends))}",
        f"GPU layer offload fraction: {next(iter(offload_modes)):.3f}",
        "Both models used their GGUF-native pooling settings. Each cold index used a separate SQLite file.", "",
        "## Aggregate retrieval metrics", "",
        "| Model | Questions | P@5 | Recall@5 | MRR |",
        "|---|---:|---:|---:|---:|",
    ]
    for model_name in models:
        metrics = overall[model_name]
        lines.append(
            f"| {model_name} | {metrics['questions']} | {metrics['precision_at_5']:.3f} "
            f"| {metrics['recall_at_5']:.3f} | {metrics['mrr']:.3f} |"
        )
    lines.extend(["", "## Indexing profile", "",
                  "| Model | Repository | Chunks | Model calls | Pooling enum | Embedding time (s) | Wall time (s) | Backend | GPU layers |",
                  "|---|---|---:|---:|---:|---:|---:|---|---:|"])
    for model_name, by_repo in runs.items():
        for repo_name, run in by_repo.items():
            profile = run["profile"]
            lines.append(
                f"| {model_name} | {repo_name} | {profile['chunks_created']} "
                f"| {profile['embedding_model_calls']} "
                f"| {profile['embedding_pooling_type']} "
                f"| {profile['chunk_embedding_ms'] / 1000:.2f} "
                f"| {run['wall_seconds']:.2f} | {run['embedding_backend']} "
                f"| {run['gpu_offload_fraction']:.3f} |"
            )
    lines.extend(["", "## Per-repository retrieval metrics", "",
                  "| Model | Repository | Questions | P@5 | Recall@5 | MRR |",
                  "|---|---|---:|---:|---:|---:|"])
    for model_name, by_repo in runs.items():
        for repo_name, run in by_repo.items():
            m = run["metrics"]
            lines.append(
                f"| {model_name} | {repo_name} | {m['questions']} | "
                f"{m['precision_at_5']:.3f} | {m['recall_at_5']:.3f} | {m['mrr']:.3f} |"
            )
    lines.extend([
        "", "## Reproducibility and limits", "",
        "The detailed logs, per-run llama.cpp logs, profiles, model paths, snapshot SHAs, and per-question rankings are in `metrics.json` and adjacent run artifacts.",
        "A comparison is controlled only if every run reports the same backend; inspect the table above. Model quantization differs (Jina Q4_K_M, BGE Q8_0), as in the shipped local GGUF files. The benchmark contains 18 authored evidence labels across two bounded snapshots; it is not a user study or a full-repository quality claim.",
        "",
    ])
    (args.output_dir / "report.md").write_text("\n".join(lines))
    print(f"\nWrote {json_path} and {args.output_dir / 'report.md'}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
