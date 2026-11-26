#!/usr/bin/env python3
"""Run vector, keyword, and hybrid retrieval with/without GitTrace reranking."""

from __future__ import annotations

import argparse
import json
import re
import subprocess
import sys
import time
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
DATASET = Path(__file__).with_name("questions.jsonl")
MODES = ("vector", "keyword", "hybrid")
TOP_K = 5
RESULT_RE = re.compile(
    r"^\s*(\d+)\.\s+(.+):(\d+)-(\d+)\s+.*?\bcommit=([0-9a-f]{40})\b",
    re.MULTILINE,
)


def head_sha(repository: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(repository), "rev-parse", "HEAD"], text=True
    ).strip()


def load_questions() -> list[dict]:
    return [json.loads(line) for line in DATASET.read_text().splitlines() if line.strip()]


def parse_outputs(stdout: str, questions: list[dict]) -> dict[str, list[dict]]:
    headings = list(re.finditer(r"^Query: (.*)$", stdout, re.MULTILINE))
    ids = {q["question"]: q["id"] for q in questions}
    parsed: dict[str, list[dict]] = {}
    for i, heading in enumerate(headings):
        end = headings[i + 1].start() if i + 1 < len(headings) else len(stdout)
        query = heading.group(1)
        if query not in ids:
            raise RuntimeError(f"Unknown query in CLI output: {query}")
        parsed[ids[query]] = [
            {"rank": int(m[1]), "path": m[2], "start_line": int(m[3]),
             "end_line": int(m[4]), "commit": m[5]}
            for m in RESULT_RE.finditer(stdout[heading.end():end])
        ]
    missing = set(ids.values()) - set(parsed)
    if missing:
        raise RuntimeError(f"Missing CLI outputs for question(s): {', '.join(sorted(missing))}")
    return parsed


def query_sections(stdout: str) -> dict[str, str]:
    headings = list(re.finditer(r"^Query: (.*)$", stdout, re.MULTILINE))
    sections = {}
    for i, heading in enumerate(headings):
        end = headings[i + 1].start() if i + 1 < len(headings) else len(stdout)
        sections[heading.group(1)] = stdout[heading.end():end]
    return sections


def score_question(question: dict, candidates: list[dict], snapshot: str) -> dict:
    top = candidates[:TOP_K]
    covered: set[int] = set()
    first_rank = None
    for result in top:
        matching = [
            i for i, qrel in enumerate(question["qrels"])
            if qrel["relevance"] > 0
            and result["commit"] == snapshot
            and result["path"] == qrel["path"]
            and result["start_line"] <= qrel["end_line"]
            and result["end_line"] >= qrel["start_line"]
        ]
        result["relevant"] = bool(matching)
        result["matched_qrels"] = matching
        covered.update(matching)
        if matching and first_rank is None:
            first_rank = result["rank"]
    relevant_qrels = sum(q["relevance"] > 0 for q in question["qrels"])
    return {
        "question_id": question["id"], "repo": question["repo"],
        "retrieved": top,
        "precision_at_5": sum(r["relevant"] for r in top) / TOP_K,
        "recall_at_5": len(covered) / relevant_qrels if relevant_qrels else 0.0,
        "reciprocal_rank": 1.0 / first_rank if first_rank else 0.0,
        "first_relevant_rank": first_rank,
    }


def aggregate(rows: list[dict]) -> dict:
    n = len(rows)
    return {
        "questions": n,
        "precision_at_5": sum(r["precision_at_5"] for r in rows) / n,
        "recall_at_5": sum(r["recall_at_5"] for r in rows) / n,
        "mrr": sum(r["reciprocal_rank"] for r in rows) / n,
    }


def run_cli(command: list[str], log: Path, label: str) -> str:
    print(f"  Running {label}", flush=True)
    start = time.perf_counter()
    proc = subprocess.Popen(command, cwd=ROOT, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)
    parts: list[str] = []
    assert proc.stdout is not None
    with log.open("w") as stream:
        for line in proc.stdout:
            parts.append(line)
            stream.write(line)
            message = line.strip()
            if message.startswith("Query: "):
                print(f"    {message}", flush=True)
            elif message.startswith(("Embedding:", "Embedding complete:", "Index ready:", "Loaded ")):
                print(f"    {message}", flush=True)
    code = proc.wait()
    print(f"  Finished {label} in {time.perf_counter() - start:.1f}s (exit {code})", flush=True)
    if code:
        raise RuntimeError(f"GitTrace failed for {label}; see {log}")
    return "".join(parts)


def make_report(path: Path, data: dict, questions: list[dict]) -> None:
    lines = [
        "# Two-repository retrieval benchmark results", "",
        f"Run date: {data['metadata']['run_date']}",
        f"GitTrace revision: `{data['metadata']['gittrace_sha']}`",
        f"llama.cpp upstream revision: `{data['metadata']['llama_upstream_sha']}`",
        f"llama.cpp source snapshot revision: `{data['metadata']['llama_snapshot_sha']}`", "",
        (f"Reranker: Gemini `{data['metadata']['reranker_model']}` at `{data['metadata']['reranker_url']}`; calls are paced for the configured free-tier rate limit."
         if data["metadata"].get("reranking_run") else "Reranking was not run; only local retrieval results are included."), "",
        "The relevance labels are source-backed evidence spans authored for this benchmark, not independently adjudicated judgments. Reranking uses a ten-candidate pool and displays up to five accepted results. No-reranking displays the top five directly. Query expansion is off.", "",
        "## Overall metrics", "",
        "| Mode | Reranking | Questions | Precision@5 | Recall@5 | MRR |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for mode in MODES:
        for rerank in (False, True):
            key = f"{mode}_{'rerank' if rerank else 'no_rerank'}"
            m = data["aggregates"]["overall"].get(key)
            values = (f"{m['questions']} | {m['precision_at_5']:.3f} | {m['recall_at_5']:.3f} | {m['mrr']:.3f}"
                      if m else "Not run | — | — | —")
            lines.append(f"| {mode} | {'Yes' if rerank else 'No'} | {values} |")
    for repo in sorted({q["repo"] for q in questions}):
        lines.extend(["", f"## {repo}", "",
                      "| Mode | Reranking | Questions | Precision@5 | Recall@5 | MRR |",
                      "|---|---:|---:|---:|---:|---:|"])
        for mode in MODES:
            for rerank in (False, True):
                key = f"{mode}_{'rerank' if rerank else 'no_rerank'}"
                m = data["aggregates"][repo].get(key)
                values = (f"{m['questions']} | {m['precision_at_5']:.3f} | {m['recall_at_5']:.3f} | {m['mrr']:.3f}"
                          if m else "Not run | — | — | —")
                lines.append(f"| {mode} | {'Yes' if rerank else 'No'} | {values} |")
    lines.extend([
        "", "## Metric definitions", "",
        "- Precision@5 divides relevant returned chunks by five, including any unfilled positions after reranking filters candidates.",
        "- Recall@5 counts each labeled evidence span once if any returned chunk overlaps it.",
        "- MRR is the mean reciprocal rank of the first relevant result; a miss contributes zero.",
        "- These metrics evaluate recovery of the designated evidence, not whether a participant can synthesize a correct answer.",
        "- `metrics.json` and the condition logs in `outputs/` retain per-question judgments and full CLI output.",
        "- Any `Not run` reranking rows are intentionally blank; repository snippets were not sent to the external reranker.", "",
    ])
    path.write_text("\n".join(lines))


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gittrace-snapshot", type=Path, default=ROOT)
    parser.add_argument("--binary", type=Path, default=ROOT / "build-release/GitTrace")
    parser.add_argument("--llama-snapshot", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, default=Path(__file__).with_name("results"))
    parser.add_argument("--laya-url", default="http://127.0.0.1:8787/rerank")
    parser.add_argument("--reranker-model", default="gemini-3.5-flash-lite")
    parser.add_argument("--no-rerank-only", action="store_true",
                        help="Run local retrieval baselines without sending snippets to a reranker API")
    args = parser.parse_args()

    questions = load_questions()
    repos = {"gittrace": args.gittrace_snapshot.resolve(), "llama_cpp": args.llama_snapshot.resolve()}
    shas = {name: head_sha(repo) for name, repo in repos.items()}
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    groups = {name: [q for q in questions if q["repo"] == name] for name in repos}
    runs: dict[str, dict] = {}

    for repo_name, repo_path in repos.items():
        repo_questions = groups[repo_name]
        print(f"\n{repo_name}: {len(repo_questions)} questions at {shas[repo_name]}", flush=True)
        for mode in MODES:
            for rerank in ((False,) if args.no_rerank_only else (False, True)):
                state = "rerank" if rerank else "no_rerank"
                label = f"{repo_name}/{mode}/{state}"
                log = output / f"{repo_name}-{mode}-{state}.log"
                profile = output / f"{repo_name}-{mode}-{state}-profile.json"
                command = [str(args.binary.resolve()), "--repo", str(repo_path),
                           "--retrieval-mode", mode, "--query-format", "instructed",
                           "--top-k", str(TOP_K), "--no-interactive", "--profile", str(profile)]
                if rerank:
                    command.extend(["--laya-url", args.laya_url])
                else:
                    command.append("--no-laya")
                for q in repo_questions:
                    command.extend(["--query", q["question"]])
                stdout = run_cli(command, log, label)
                parsed = parse_outputs(stdout, repo_questions)
                retry_logs = []
                if rerank:
                    sections = query_sections(stdout)
                    failed = [q for q in repo_questions
                              if "Warning: could not reach the reranker" in sections.get(q["question"], "")]
                    for question in failed:
                        recovered = False
                        for attempt in range(1, 4):
                            retry_label = f"{label}/retry-{question['id']}-{attempt}"
                            retry_log = output / f"{repo_name}-{mode}-rerank-retry-{question['id']}-{attempt}.log"
                            retry_profile = output / f"{repo_name}-{mode}-rerank-retry-{question['id']}-{attempt}-profile.json"
                            retry_command = [str(args.binary.resolve()), "--repo", str(repo_path),
                                             "--retrieval-mode", mode, "--query-format", "instructed",
                                             "--top-k", str(TOP_K), "--no-interactive", "--profile", str(retry_profile),
                                             "--laya-url", args.laya_url, "--query", question["question"]]
                            retry_stdout = run_cli(retry_command, retry_log, retry_label)
                            retry_logs.append(retry_log.name)
                            retry_section = query_sections(retry_stdout).get(question["question"], "")
                            if "Warning: could not reach the reranker" not in retry_section:
                                parsed[question["id"]] = parse_outputs(retry_stdout, [question])[question["id"]]
                                recovered = True
                                break
                        if not recovered:
                            raise RuntimeError(f"Reranker failed after three retries for {label}/{question['id']}")
                scores = [score_question(q, parsed[q["id"]], shas[repo_name]) for q in repo_questions]
                runs[label] = {
                    "repo": repo_name, "mode": mode, "reranking": rerank,
                    "snapshot_sha": shas[repo_name], "output_log": log.name,
                    "profile": profile.name, "retry_logs": retry_logs,
                    "fallback_retries": len(retry_logs),
                    "questions": scores, "aggregate": aggregate(scores),
                }

    per_repo: dict[str, dict] = defaultdict(dict)
    overall_rows: dict[str, list] = defaultdict(list)
    for run in runs.values():
        key = f"{run['mode']}_{'rerank' if run['reranking'] else 'no_rerank'}"
        per_repo[run["repo"]][key] = run["aggregate"]
        overall_rows[key].extend(run["questions"])
    overall = {key: aggregate(rows) for key, rows in overall_rows.items()}
    metadata = {
        "run_date": time.strftime("%Y-%m-%d"),
        "gittrace_sha": shas["gittrace"],
        "llama_upstream_sha": "a6ea155d3d38b3f6f43d0c0dc29c2d592413ef44",
        "llama_snapshot_sha": shas["llama_cpp"],
        "reranker_model": args.reranker_model,
        "reranking_run": not args.no_rerank_only,
        "top_k": TOP_K, "query_expansion": False, "reranker_url": args.laya_url,
    }
    result = {"metadata": metadata, "questions": questions,
              "aggregates": {"overall": overall, **dict(per_repo)}, "runs": runs}
    (output / "metrics.json").write_text(json.dumps(result, indent=2) + "\n")
    make_report(output / "report.md", result, questions)
    print(f"\nWrote {output / 'report.md'}", flush=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"benchmark error: {error}", file=sys.stderr)
        raise SystemExit(1)
