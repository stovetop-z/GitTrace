# Local PyTorch embedding run

## Completed run

- Model: `jinaai/jina-embeddings-v2-base-code`
- Source: curated 15-file `llama.cpp` snapshot based on upstream `a6ea155d3d38b3f6f43d0c0dc29c2d592413ef44`
- Device and dtype: Apple MPS, float16
- Batch size: 4
- Maximum sequence length: 2,048 tokens
- Inputs: 1,135 byte-bounded chunks from 15 files
- Model load: 20.46 seconds
- Encoding: 69.98 seconds, or 16.22 chunks/second
- Output vectors: `jina-code-index.npz` in the temporary benchmark directory

The model card describes Jina v2 base code as a 161M-parameter code embedder trained for English and 30 programming languages. It recommends mean pooling. The matching Q4_K_M GGUF is now the GitTrace default; it returns 768-dimensional vectors using mean pooling.

## Retrieval quality

The same Jina PyTorch vectors were evaluated against the current 18 labeled questions, with cosine vector search and no reranking:

| Questions | Precision@5 | Recall@5 | MRR |
| ---: | ---: | ---: | ---: |
| 18 | 0.233 | 0.722 | 0.415 |

GitTrace's nine questions scored P@5 0.156, Recall@5 0.556, and MRR 0.378. The nine llama.cpp questions scored P@5 0.311, Recall@5 0.889, and MRR 0.452. Full rankings are in [`results/jina-vector-metrics.json`](results/jina-vector-metrics.json).

This is a useful initial baseline, not a controlled win over BGE: the BGE indexing runs were stopped before completion, so there is no matching BGE metric set. The 18 questions and two repositories are also a small sample, and these metrics exclude reranking.

## C++ compatibility smoke check

The Q4_K_M GGUF loaded through GitTrace's normal indexing path on a one-file, one-commit fixture. GitTrace embedded one chunk in one decode call and returned the expected `demo.cpp` snippet for a natural-language query. The sandbox selected CPU fallback, and one chunk is too little to treat as a throughput benchmark. This validates model loading, mean pooling, vector storage, and query retrieval in the C++ application.

## Comparison limits

The earlier C++ BGE batched run used the same 15 source files but stopped at 128 chunks after 179.3 seconds of indexing, with CPU fallback and 32 decode calls. Its chunk boundaries come from Tree-sitter; the Python script uses newline-aware 1,500-byte chunks and produced 1,135 inputs. The Python run also used MPS, while that C++ run used CPU. These rates are therefore directional and do not isolate the model framework.

The BGE PyTorch runs were stopped before completion. Batch size 10 was slower in its partial run than batch size 4; no complete BGE throughput or retrieval figure was recorded.

## Reproduction

See [`pytorch_bge_index.py`](pytorch_bge_index.py) and [`requirements-pytorch-bge.txt`](requirements-pytorch-bge.txt). The run used `--model jinaai/jina-embeddings-v2-base-code --batch-size 4 --max-seq-length 2048 --device auto --trust-remote-code`.
