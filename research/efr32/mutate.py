#!/usr/bin/env python3
"""Generate a bounded deterministic offline mutation corpus."""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import random


MAX_INPUT_BYTES = 512


def mutations(seed: bytes, random_seed: int):
    yield "length", bytes([0]) + seed[1:]
    yield "length", bytes([(len(seed) - 2) & 0x7F]) + seed[1:]
    for value in (0, 1, 4, 7, 11, 255):
        if len(seed) > 16:
            changed = bytearray(seed)
            changed[16] = value
            yield "type", bytes(changed)
    for offset, values in ((3, (0, 0x7F, 0xFF)), (20, (0, 1, 0xFF))):
        if len(seed) > offset:
            for value in values:
                changed = bytearray(seed)
                changed[offset] = value
                yield "counter" if offset == 3 else "id", bytes(changed)
    for cut in sorted({0, 1, 2, 3, 8, 16, 20, len(seed) // 2, len(seed) - 1}):
        if 0 <= cut < len(seed):
            yield "truncate", seed[:cut]
    for suffix in (b"\x00", b"\xff", bytes(8), seed[:16]):
        yield "extend", seed + suffix
    yield "repeat", seed + seed
    yield "repeat", seed[:20] + seed[20:] * 3

    rng = random.Random(random_seed)
    for _ in range(128):
        changed = bytearray(seed)
        if not changed:
            changed.append(rng.randrange(256))
        count = 1 + rng.randrange(min(8, len(changed)))
        for _ in range(count):
            changed[rng.randrange(len(changed))] ^= 1 << rng.randrange(8)
        dimension = ("length", "type", "counter", "id")[rng.randrange(4)]
        yield dimension, bytes(changed)


def generate(seed_path: Path, output: Path, limit: int, random_seed: int) -> dict:
    seed = seed_path.read_bytes()
    if len(seed) > MAX_INPUT_BYTES:
        raise SystemExit(f"seed exceeds {MAX_INPUT_BYTES} bytes")
    output.mkdir(parents=True, exist_ok=True)
    seen: set[str] = set()
    cases = []
    for dimension, content in mutations(seed, random_seed):
        if len(cases) >= limit:
            break
        if len(content) > MAX_INPUT_BYTES:
            continue
        digest = hashlib.sha256(content).hexdigest()
        if digest in seen:
            continue
        seen.add(digest)
        filename = f"cases/{len(cases):03d}-{dimension}-{digest[:16]}.bin"
        destination = output / filename
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_bytes(content)
        cases.append({"dimension": dimension, "path": filename,
                      "sha256": digest, "size": len(content)})
    manifest = {
        "schema": "bh61-efr32-mutation-corpus/v1",
        "scope": "offline-only",
        "random_seed": random_seed,
        "seed": {"name": seed_path.name, "sha256": hashlib.sha256(seed).hexdigest(),
                 "size": len(seed)},
        "limits": {"max_cases": limit, "max_input_bytes": MAX_INPUT_BYTES},
        "cases": cases,
    }
    (output / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--limit", type=int, default=96)
    parser.add_argument("--random-seed", type=lambda value: int(value, 0), default=0x0B61)
    args = parser.parse_args()
    if not 1 <= args.limit <= 4096:
        raise SystemExit("limit must be 1..4096")
    generate(args.seed, args.output, args.limit, args.random_seed)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
