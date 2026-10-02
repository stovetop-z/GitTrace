"""Local Laya reranking service for GitTrace search candidates."""

from __future__ import annotations

import logging
import os
import time
from contextlib import asynccontextmanager
from threading import Lock
from typing import Literal

from fastapi import FastAPI, HTTPException, Request
from pydantic import BaseModel, Field

logger = logging.getLogger("gittrace.laya")

MAX_CANDIDATES = 64
DECISION_ORDER = {"direct": 0, "related": 1, "irrelevant": 2}

QUESTIONS = {
    "relevance": {
        "type": "choice",
        "instructions": (
            "Judge how useful this retrieved code or document chunk is for answering "
            "the user's query. Base the decision on the chunk's meaning, not shared "
            "words alone. Choose direct when it contains the answer or concrete "
            "evidence, related when it gives useful context, and irrelevant when "
            "the match is incidental or unrelated."
        ),
        "criteria": {
            "direct": "Directly answers the query or contains concrete evidence for it.",
            "related": "Provides useful context but does not directly answer the query.",
            "irrelevant": "Does not help answer the query; any match is incidental.",
        },
    }
}


class Candidate(BaseModel):
    id: str = Field(min_length=1, max_length=256)
    path: str = Field(min_length=1, max_length=2048)
    text: str = Field(min_length=1, max_length=12000)
    similarity: float | None = None
    commit_sha: str | None = Field(default=None, max_length=128)
    start_line: int | None = Field(default=None, ge=0)
    end_line: int | None = Field(default=None, ge=0)


class RerankRequest(BaseModel):
    query: str = Field(min_length=1, max_length=2000)
    candidates: list[Candidate] = Field(min_length=1, max_length=MAX_CANDIDATES)
    # "related" keeps direct and related results; "direct" keeps only direct hits.
    min_relevance: Literal["direct", "related", "any"] = "related"
    # English Laya checkpoint context limit. Keep configurable for other checkpoints.
    max_len: int = Field(default=256, ge=64, le=512)


@asynccontextmanager
async def lifespan(app: FastAPI):
    # Keep Laya and its checkpoint loaded between HTTP requests.
    try:
        from laya import load

        model_id = os.environ.get("GITTRACE_LAYA_MODEL", "convaiinnovations/laya")
        subfolder = os.environ.get("GITTRACE_LAYA_SUBFOLDER")
        if subfolder:
            app.state.agent = load(model_id, subfolder=subfolder)
        else:
            app.state.agent = load(model_id)
        app.state.inference_lock = Lock()
        app.state.model_id = model_id
        logger.info("Loaded Laya checkpoint %s", model_id)
    except Exception as exc:
        logger.exception("Could not load the Laya checkpoint")
        raise RuntimeError("Could not load Laya; install requirements-laya.txt and check model access") from exc
    yield


app = FastAPI(
    title="GitTrace Laya Reranker",
    description="Classify vector-search candidates for relevance to a GitTrace query.",
    version="0.1.0",
    lifespan=lifespan,
)


@app.get("/health")
def health(request: Request) -> dict[str, object]:
    agent = getattr(request.app.state, "agent", None)
    return {"status": "ready" if agent is not None else "starting",
            "model": getattr(request.app.state, "model_id", None)}


@app.post("/rerank")
def rerank(body: RerankRequest, request: Request) -> dict[str, object]:
    agent = getattr(request.app.state, "agent", None)
    if agent is None:
        raise HTTPException(status_code=503, detail="Laya model is not ready")

    states = []
    for candidate in body.candidates:
        state = (
            f"User query:\n{body.query}\n\n"
            f"Candidate file: {candidate.path}\n"
            f"Candidate lines: {candidate.start_line or 0}-{candidate.end_line or 0}\n"
            f"Candidate content:\n{candidate.text}"
        )
        states.append({"body": state})

    try:
        # Laya supports batched predictions and returns answers in input order.
        started = time.perf_counter()
        with request.app.state.inference_lock:
            predictions = agent.predict_batch(
                states,
                QUESTIONS,
                batch_size=min(MAX_CANDIDATES, len(states)),
                max_len=body.max_len,
            )
        logger.info("Reranked %d candidates in %.2fs", len(states), time.perf_counter() - started)
    except Exception as exc:
        logger.exception("Laya reranking failed")
        raise HTTPException(status_code=502, detail="Laya reranking failed") from exc

    min_rank = {"direct": 0, "related": 1, "any": 2}[body.min_relevance]
    results = []
    for candidate, prediction in zip(body.candidates, predictions):
        answer = prediction.get("answers", {}).get("relevance", {})
        decision = str(answer.get("choice", "irrelevant")).lower()
        if decision not in DECISION_ORDER:
            decision = "irrelevant"
        confidence = answer.get("answer_confidence", answer.get("confidence", 0.0))
        try:
            confidence = float(confidence)
        except (TypeError, ValueError):
            confidence = 0.0

        results.append({
            "id": candidate.id,
            "path": candidate.path,
            "commit_sha": candidate.commit_sha,
            "start_line": candidate.start_line,
            "end_line": candidate.end_line,
            "similarity": candidate.similarity,
            "relevance": decision,
            "confidence": confidence,
            "accepted": DECISION_ORDER[decision] <= min_rank,
        })

    results.sort(key=lambda item: (
        DECISION_ORDER[item["relevance"]],
        -item["confidence"],
        -(item["similarity"] if item["similarity"] is not None else -1.0),
    ))
    return {
        "query": body.query,
        "candidate_count": len(body.candidates),
        "accepted_count": sum(1 for item in results if item["accepted"]),
        "results": results,
    }


if __name__ == "__main__":
    import uvicorn

    host = os.environ.get("GITTRACE_LAYA_HOST", "127.0.0.1")
    port = int(os.environ.get("GITTRACE_LAYA_PORT", "8787"))
    uvicorn.run(app, host=host, port=port)
