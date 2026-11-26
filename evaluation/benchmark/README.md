# Two-repository retrieval benchmark

This benchmark contains 18 natural-language questions with source-backed evidence labels: nine for GitTrace and nine for `llama.cpp`. It compares vector, keyword, and hybrid retrieval, each with and without the GitTrace reranking stage.

## Question labels

Each question has one binary-relevant evidence span: repository-relative path, inclusive line range, and a short rationale. A result is relevant when its path matches, its line range overlaps the evidence span, and its commit matches the evaluated repository snapshot. The labels were authored from the source code for this benchmark; they have not been independently judged. They measure retrieval of the designated implementation evidence, not general answer correctness.

## Corpus snapshots

- GitTrace is evaluated from a filtered clone at source revision `43a49a5ab34127612f7926a775636172467c401d`. The clone preserves its Git history and adds only a benchmark `.gittraceignore` commit that excludes documentation and evaluation artifacts, preventing the benchmark's own query notes from leaking into retrieval.
- `llama.cpp` is evaluated from a one-commit, curated source snapshot based on upstream commit `a6ea155d3d38b3f6f43d0c0dc29c2d592413ef44`. It contains the eight files with labeled evidence plus seven nearby implementation files as distractors. This bounded slice was chosen after indexing the broader source tree proved too slow under the available CPU fallback. It evaluates retrieval within these task areas, not the full upstream checkout. The runner records the synthetic snapshot commit SHA in its output.
- The snapshot's `.gittraceignore` excludes documentation, tests, and model data. The llama.cpp result applies only to the 15 files in this curated snapshot.

## Metrics

- Precision@5 = relevant retrieved chunks in the first five positions divided by five. If fewer than five pass reranking, the remaining positions count as non-relevant.
- Recall@5 = gold evidence spans retrieved at least once in the first five divided by the number of gold spans (one per question).
- MRR = the mean reciprocal rank of the first relevant result; an unanswered question contributes zero.

Metrics are macro-averaged over questions. Report overall and per-repository values. The reranking condition retrieves ten candidates, reranks and filters them, then displays at most five. The no-reranking condition displays the retrieval mode's top five directly. Query expansion is off in both conditions.

## Reproduction

1. Build the current checkout in Release mode.
2. Prepare filtered snapshots with `prepare_gittrace_snapshot.sh` and `prepare_llama_snapshot.sh`.
3. Prepare the rate-limited Gemini service copy with `python3 evaluation/benchmark/prepare_rate_limited_reranker.py`, then start it from `/private/tmp/gittrace-benchmark/service` using the project's `laya_service:app`, `GITTRACE_RERANKER=gemini`, and `GITTRACE_GEMINI_MODEL=gemini-3.5-flash-lite`. Its eight-second inter-batch gap keeps the run under the observed free-tier request limit; the delay does not change ranking decisions. The runner fails rather than silently counting a reranker fallback as a successful reranked result.
4. Run `python3 evaluation/benchmark/run_benchmark.py --gittrace-snapshot /private/tmp/gittrace-benchmark/gittrace --llama-snapshot /private/tmp/gittrace-benchmark/llama.cpp`.

The runner saves raw CLI outputs, profiles, per-question metrics, aggregate metrics, and a Markdown report under `evaluation/benchmark/results/`.
