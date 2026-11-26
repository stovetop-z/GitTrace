#!/usr/bin/env python3
"""Score local PyTorch code embeddings against the benchmark evidence labels."""

from __future__ import annotations

import argparse
import json
from pathlib import Path


def aggregate(rows: list[dict]) -> dict:
    count = len(rows)
    return {
        "questions": count,
        "precision_at_5": sum(row["precision_at_5"] for row in rows) / count,
        "recall_at_5": sum(row["recall_at_5"] for row in rows) / count,
        "mrr": sum(row["reciprocal_rank"] for row in rows) / count,
    }


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--gittrace-index", type=Path, required=True)
    parser.add_argument("--llama-index", type=Path, required=True)
    parser.add_argument("--model", default="jinaai/jina-embeddings-v2-base-code")
    parser.add_argument("--device", default="mps")
    parser.add_argument("--max-seq-length", type=int, default=2048)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    import numpy as np
    import torch
    from sentence_transformers import SentenceTransformer

    questions_path = Path(__file__).with_name("questions.jsonl")
    questions = [json.loads(line) for line in questions_path.read_text().splitlines() if line.strip()]
    model = SentenceTransformer(
        args.model,
        device=args.device,
        trust_remote_code=True,
        model_kwargs={"torch_dtype": torch.float16 if args.device in ("mps", "cuda") else torch.float32},
    )
    model.max_seq_length = args.max_seq_length
    model.eval()
    query_vectors = model.encode(
        [question["question"] for question in questions],
        batch_size=8,
        show_progress_bar=True,
        convert_to_numpy=True,
        normalize_embeddings=True,
    )

    index_paths = {"gittrace": args.gittrace_index, "llama_cpp": args.llama_index}
    indexes = {}
    for repo, path in index_paths.items():
        archive = np.load(path)
        vectors = archive["embeddings"].astype(np.float32)
        norms = np.linalg.norm(vectors, axis=1, keepdims=True)
        vectors /= np.maximum(norms, 1e-12)
        indexes[repo] = {
            "vectors": vectors,
            "paths": archive["paths"],
            "start_lines": archive["start_lines"],
            "end_lines": archive["end_lines"],
        }

    rows = []
    for query_index, question in enumerate(questions):
        index = indexes[question["repo"]]
        similarities = index["vectors"] @ query_vectors[query_index].astype(np.float32)
        order = np.argsort(-similarities, kind="stable")[:5]
        relevant_ranks = []
        retrieved = []
        for rank, result_index in enumerate(order, start=1):
            result_path = str(index["paths"][result_index])
            result_start = int(index["start_lines"][result_index])
            result_end = int(index["end_lines"][result_index])
            relevant = any(
                qrel["relevance"] > 0
                and result_path == qrel["path"]
                and result_start <= qrel["end_line"]
                and result_end >= qrel["start_line"]
                for qrel in question["qrels"]
            )
            if relevant:
                relevant_ranks.append(rank)
            retrieved.append({
                "rank": rank,
                "path": result_path,
                "start_line": result_start,
                "end_line": result_end,
                "cosine": float(similarities[result_index]),
                "relevant": relevant,
            })
        rank = min(relevant_ranks) if relevant_ranks else None
        gold_count = sum(qrel["relevance"] > 0 for qrel in question["qrels"])
        recall = (1.0 if relevant_ranks else 0.0) if gold_count == 1 else min(
            len(relevant_ranks) / gold_count, 1.0
        )
        rows.append({
            "question_id": question["id"],
            "repo": question["repo"],
            "retrieved": retrieved,
            "precision_at_5": sum(item["relevant"] for item in retrieved) / 5,
            "recall_at_5": recall,
            "first_relevant_rank": rank,
            "reciprocal_rank": 1.0 / rank if rank else 0.0,
        })

    overall = aggregate(rows)
    by_repo = {
        repo: aggregate([row for row in rows if row["repo"] == repo])
        for repo in index_paths
    }
    result = {
        "model": args.model,
        "device": args.device,
        "max_seq_length": args.max_seq_length,
        "retrieval": "cosine vector search, no reranking",
        "overall": overall,
        "by_repo": by_repo,
        "questions": rows,
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n")
    print(json.dumps({"overall": overall, "by_repo": by_repo, "output": str(args.output)}, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
