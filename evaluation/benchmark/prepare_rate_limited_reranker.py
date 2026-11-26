#!/usr/bin/env python3
"""Create a temporary Gemini service copy paced for free-tier request limits."""

from __future__ import annotations

import argparse
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LOOP = "    for offset in range(0, len(body.candidates), GEMINI_BATCH_SIZE):\n"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", type=Path,
                        default=Path("/private/tmp/gittrace-benchmark/service/laya_service.py"))
    parser.add_argument("--gap-seconds", type=float, default=8.0)
    args = parser.parse_args()
    source = (ROOT / "laya_service.py").read_text()
    if source.count(LOOP) != 1:
        raise RuntimeError("Could not find the expected Gemini candidate-batch loop")
    replacement = LOOP + f"        time.sleep({args.gap_seconds:.1f})\n"
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(source.replace(LOOP, replacement, 1))
    print(f"Wrote {args.output} with a {args.gap_seconds:.1f}s gap before each Gemini candidate batch")


if __name__ == "__main__":
    main()
