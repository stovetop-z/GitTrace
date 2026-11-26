#!/usr/bin/env python3
"""Measure local SentenceTransformers indexing throughput for a code embedder."""

from __future__ import annotations

import argparse
import json
import os
import subprocess
import time
from pathlib import Path


CODE_SUFFIXES = {
    ".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx",
    ".py", ".rs", ".go", ".java", ".js", ".jsx", ".ts", ".tsx",
    ".cs", ".swift", ".kt", ".m", ".mm", ".sh",
}


def tracked_files(repo: Path) -> list[Path]:
    paths = subprocess.check_output(
        ["git", "-C", str(repo), "ls-files", "-z"], stderr=subprocess.DEVNULL
    ).split(b"\0")
    return [repo / os.fsdecode(p) for p in paths if p and Path(os.fsdecode(p)).suffix.lower() in CODE_SUFFIXES]


def make_chunks(path: Path, repo: Path, max_bytes: int) -> list[dict]:
    data = path.read_bytes()
    chunks: list[dict] = []
    offset = 0
    start_line = 1
    while offset < len(data):
        end = min(len(data), offset + max_bytes)
        if end < len(data):
            newline = data.rfind(b"\n", offset, end)
            if newline >= offset:
                end = newline + 1
        if end <= offset:
            end = min(len(data), offset + max_bytes)
        text = data[offset:end].decode("utf-8", errors="replace")
        line_count = text.count("\n")
        end_line = start_line + line_count - int(line_count > 0 and text.endswith("\n"))
        chunks.append({
            "path": path.relative_to(repo).as_posix(),
            "start_line": start_line,
            "end_line": max(start_line, end_line),
            "text": text,
        })
        start_line += line_count
        offset = end
    return chunks


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("repo", type=Path)
    parser.add_argument("--model", default="BAAI/bge-code-v1")
    parser.add_argument("--batch-size", type=int, default=4)
    parser.add_argument("--max-bytes", type=int, default=1500)
    parser.add_argument("--max-seq-length", type=int, default=2048)
    parser.add_argument("--device", choices=("auto", "cpu", "mps", "cuda"), default="auto")
    parser.add_argument("--trust-remote-code", action="store_true")
    parser.add_argument("--output", type=Path, default=Path("bge-pytorch-index.npz"))
    args = parser.parse_args()

    import numpy as np
    import torch
    from sentence_transformers import SentenceTransformer

    repo = args.repo.resolve()
    files = tracked_files(repo)
    chunking_started = time.perf_counter()
    chunks = [chunk for path in files for chunk in make_chunks(path, repo, args.max_bytes)]
    chunking_seconds = time.perf_counter() - chunking_started
    if not chunks:
        raise SystemExit(f"No supported source files found under {repo}")

    if args.device == "auto":
        if torch.cuda.is_available():
            device = "cuda"
        elif getattr(torch.backends, "mps", None) and torch.backends.mps.is_available():
            device = "mps"
        else:
            device = "cpu"
    else:
        device = args.device

    dtype = torch.float16 if device in ("mps", "cuda") else torch.float32
    model_started = time.perf_counter()
    model = SentenceTransformer(
        args.model,
        device=device,
        trust_remote_code=args.trust_remote_code,
        model_kwargs={"torch_dtype": dtype},
    )
    model.max_seq_length = args.max_seq_length
    model.eval()
    model_load_seconds = time.perf_counter() - model_started
    print(f"Loaded {args.model} on {device} ({dtype}); encoding {len(chunks)} chunks.", flush=True)

    encode_started = time.perf_counter()
    vectors = model.encode(
        [chunk["text"] for chunk in chunks],
        batch_size=args.batch_size,
        show_progress_bar=True,
        convert_to_numpy=True,
        normalize_embeddings=False,
    )
    encode_seconds = time.perf_counter() - encode_started

    args.output.parent.mkdir(parents=True, exist_ok=True)
    np.savez_compressed(
        args.output,
        embeddings=vectors,
        paths=np.asarray([chunk["path"] for chunk in chunks]),
        start_lines=np.asarray([chunk["start_line"] for chunk in chunks], dtype=np.uint32),
        end_lines=np.asarray([chunk["end_line"] for chunk in chunks], dtype=np.uint32),
    )
    summary = {
        "model": args.model,
        "repo": str(repo),
        "files": len(files),
        "chunks": len(chunks),
        "batch_size": args.batch_size,
        "max_seq_length": args.max_seq_length,
        "device": device,
        "dtype": str(dtype),
        "chunking_seconds": chunking_seconds,
        "model_load_seconds": model_load_seconds,
        "encoding_seconds": encode_seconds,
        "chunks_per_second": len(chunks) / encode_seconds if encode_seconds else 0.0,
        "decode_batches": (len(chunks) + args.batch_size - 1) // args.batch_size,
        "index_file": str(args.output.resolve()),
    }
    print(json.dumps(summary, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
