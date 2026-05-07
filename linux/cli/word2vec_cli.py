#!/usr/bin/env python3
"""
nn20db-embedded-word2vect CLI

Interactive word / expression nearest-neighbour search using a pre-built
nn20db HNSW index. Supports local search and forwarding to an ESP32-S3
server over TCP.

Usage:
    python word2vec_cli.py \\
        --db <path-to-nn20db-dir> \\
        --word-vectors <GoogleNews-vectors-negative300.bin.gz> \\
        [--limit N] \\
        [--esp32 host:port] \\
        [--k 10] \\
        [--ef 100]

Examples:
    python word2vec_cli.py --db ~/data/word2vec_db \\
        --word-vectors ~/data/GoogleNews-vectors-negative300.bin.gz

    python word2vec_cli.py --db ~/data/word2vec_db \\
        --word-vectors ~/data/GoogleNews-vectors-negative300.bin.gz \\
        --esp32 192.168.1.42:9900 --k 10 --ef 32

Interactive expressions:
    word2vec> king
    word2vec> king - man + woman
    word2vec> quit
"""

import argparse
import gzip
import os
import socket
import struct
import sys
import time
from pathlib import Path
from typing import Dict, List, Optional, Tuple

import numpy as np

# ── nn20db Python API ────────────────────────────────────────────────────────
# The Python API lives in the nn20db-sdk repo; we try both the installed
# location and a sibling repo path.
def _find_nn20db_py() -> Optional[Path]:
    candidates = [
        Path(__file__).resolve().parent.parent.parent.parent / "api" / "python",
        Path(__file__).resolve().parent.parent.parent / "api" / "python",
    ]
    for c in candidates:
        if (c / "nn20db.py").exists():
            return c
    return None

_nn20db_api_path = _find_nn20db_py()
if _nn20db_api_path:
    sys.path.insert(0, str(_nn20db_api_path))

try:
    from nn20db import NN20Db, DatabaseConfig, LfsStorageConfig, HnswConfig, HnswLevelConfig, CacheConfig
    _HAVE_NN20DB = True
except ImportError:
    _HAVE_NN20DB = False


# ── constants ────────────────────────────────────────────────────────────────

DIM = 300
METADATA_SIZE = 32          # sizeof(word_meta_t): char word[32]
WIRE_QUERY_BYTES  = DIM * 4            # 1200 bytes: 300 × float32 LE
WIRE_RESULT_BYTES = METADATA_SIZE + 4  # 36 bytes: 32-byte word + 4-byte float32


# ── vector helpers ───────────────────────────────────────────────────────────

def normalize(vec: np.ndarray) -> np.ndarray:
    norm = np.linalg.norm(vec)
    if norm > 0:
        return vec / norm
    return vec.copy()


# ── GoogleNews .bin.gz loader ────────────────────────────────────────────────

def load_word_vectors(path: str, limit: int = 0) -> Dict[str, np.ndarray]:
    """
    Parse GoogleNews Word2Vec binary format and return word -> unit-vector dict.

    Format:
        "<count> <dim>\\n"
        [word SP dim×float32] × count
    """
    print(f"Loading word vectors from {path} ...", flush=True)
    t0 = time.time()

    opener = gzip.open if path.endswith(".gz") else open
    word_vecs: Dict[str, np.ndarray] = {}

    with opener(path, "rb") as f:
        # header
        header = f.readline().decode("utf-8").strip()
        total_words, dim = map(int, header.split())
        if dim != DIM:
            raise ValueError(f"Expected dim={DIM}, got {dim}")

        max_words = limit if limit > 0 else total_words
        print(f"  File: {total_words} words, dim={dim}. Loading {max_words}.")

        for i in range(max_words):
            # read word (terminated by space)
            word_chars = []
            while True:
                c = f.read(1)
                if not c:
                    break
                if c == b' ':
                    break
                if c == b'\n':
                    continue   # skip leading newlines between entries
                word_chars.append(c)
            if not word_chars:
                break
            word = b"".join(word_chars).decode("utf-8", errors="replace")

            raw = f.read(dim * 4)
            if len(raw) != dim * 4:
                print(f"  Short read at word {i} ({word!r})")
                break

            vec = np.frombuffer(raw, dtype=np.float32).copy()
            word_vecs[word] = normalize(vec)

            if (i + 1) % 50000 == 0:
                elapsed = time.time() - t0
                rate = (i + 1) / elapsed if elapsed > 0 else 0
                print(f"  {i+1}/{max_words}  ({rate:.0f} words/s)", flush=True)

    elapsed = time.time() - t0
    print(f"Loaded {len(word_vecs)} words in {elapsed:.1f} s")
    return word_vecs


# ── expression parser ────────────────────────────────────────────────────────

def parse_expression(
    expr: str,
    word_vecs: Dict[str, np.ndarray],
) -> Tuple[np.ndarray, List[str]]:
    """
    Parse a word arithmetic expression such as "king - man + woman".

    Returns (query_vector, list_of_tokens_used).
    Raises ValueError on unknown words or parse errors.
    """
    tokens = expr.split()
    if not tokens:
        raise ValueError("Empty expression")

    result = np.zeros(DIM, dtype=np.float32)
    sign = +1
    i = 0
    used = []

    while i < len(tokens):
        tok = tokens[i]
        if tok == "+":
            sign = +1
            i += 1
            continue
        if tok == "-":
            sign = -1
            i += 1
            continue

        if tok not in word_vecs:
            raise ValueError(f"Unknown word: '{tok}'")

        result += sign * word_vecs[tok]
        used.append(f"{'+-'[sign < 0]}{tok}" if sign < 0 else tok)
        sign = +1
        i += 1

    if np.linalg.norm(result) == 0:
        raise ValueError("Zero vector after arithmetic")

    return normalize(result), used


# ── local nn20db search ──────────────────────────────────────────────────────

def local_search(
    db: "NN20Db",
    query_vec: np.ndarray,
    k: int,
    ef: int,
) -> List[Tuple[str, float]]:
    """Search the local nn20db index. Returns list of (word, distance)."""
    results = db.search_ef(query_vec.tolist(), k=k, ef_search=ef)
    output = []
    for sr in results:
        raw_vec, raw_meta = db.get(sr.id, dimension=DIM, metadata_size=METADATA_SIZE)
        # metadata is bytes: char word[32], null-padded
        word = raw_meta[:32].rstrip(b"\x00").decode("utf-8", errors="replace")
        output.append((word, float(sr.distance)))
    return output


# ── ESP32 proxy search ───────────────────────────────────────────────────────

def esp32_search(
    host: str,
    port: int,
    query_vec: np.ndarray,
    k: int,
) -> List[Tuple[str, float]]:
    """
    Send query vector to ESP32 TCP server and receive top-k results.

    Wire protocol:
        TX: 300 × float32 LE  (1200 bytes)
        RX: k × (32-byte word + 4-byte float32)  (k × 36 bytes)
    """
    payload = query_vec.astype(np.float32).tobytes()
    assert len(payload) == WIRE_QUERY_BYTES

    with socket.create_connection((host, port), timeout=10.0) as s:
        s.sendall(payload)

        response_size = k * WIRE_RESULT_BYTES
        buf = b""
        while len(buf) < response_size:
            chunk = s.recv(response_size - len(buf))
            if not chunk:
                break
            buf += chunk

    results = []
    for i in range(0, len(buf) - WIRE_RESULT_BYTES + 1, WIRE_RESULT_BYTES):
        word_bytes = buf[i : i + METADATA_SIZE]
        dist_bytes = buf[i + METADATA_SIZE : i + METADATA_SIZE + 4]
        word = word_bytes.rstrip(b"\x00").decode("utf-8", errors="replace")
        (dist,) = struct.unpack("<f", dist_bytes)
        results.append((word, dist))
    return results


# ── result printer ───────────────────────────────────────────────────────────

def print_results(results: List[Tuple[str, float]], source: str) -> None:
    print(f"  [{source}]")
    for rank, (word, dist) in enumerate(results, 1):
        print(f"  {rank:3d}  {word:<32s}  {dist:.6f}")


# ── REPL ─────────────────────────────────────────────────────────────────────

def repl(
    db,
    word_vecs: Dict[str, np.ndarray],
    esp32_addr: Optional[Tuple[str, int]],
    k: int,
    ef: int,
) -> None:
    print()
    print("Word2Vec REPL  (type 'quit' to exit)")
    print("  Examples:  king")
    print("             king - man + woman")
    print()

    while True:
        try:
            line = input("word2vec> ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not line:
            continue
        if line.lower() in ("quit", "exit", "q"):
            break

        try:
            query_vec, used_tokens = parse_expression(line, word_vecs)
        except ValueError as e:
            print(f"  Error: {e}")
            continue

        print(f"  Query vector built from: {', '.join(used_tokens)}")

        # local search
        if db is not None:
            try:
                t0 = time.perf_counter()
                results = local_search(db, query_vec, k=k, ef=ef)
                elapsed_ms = (time.perf_counter() - t0) * 1000
                print(f"  Local search  ({elapsed_ms:.1f} ms)")
                print_results(results, "Linux nn20db")
            except Exception as exc:
                print(f"  Local search error: {exc}")

        # ESP32 search
        if esp32_addr is not None:
            host, port = esp32_addr
            try:
                t0 = time.perf_counter()
                results = esp32_search(host, port, query_vec, k=k)
                elapsed_ms = (time.perf_counter() - t0) * 1000
                print(f"  ESP32 search  ({elapsed_ms:.1f} ms)")
                print_results(results, f"ESP32 {host}:{port}")
            except Exception as exc:
                print(f"  ESP32 search error: {exc}")

        print()


# ── entry point ──────────────────────────────────────────────────────────────

def main():
    parser = argparse.ArgumentParser(
        description="Word2Vec interactive search CLI (nn20db backend)",
    )
    parser.add_argument("--db", required=True,
                        help="Path to the nn20db HNSW database directory")
    parser.add_argument("--word-vectors", required=True, metavar="FILE",
                        help="GoogleNews-vectors-negative300.bin.gz path")
    parser.add_argument("--limit", type=int, default=0, metavar="N",
                        help="Load only first N word vectors into RAM (default: all)")
    parser.add_argument("--esp32", metavar="HOST:PORT",
                        help="Optional: also send queries to ESP32 TCP server")
    parser.add_argument("--k", type=int, default=10,
                        help="Number of nearest neighbours to return (default: 10)")
    parser.add_argument("--ef", type=int, default=100,
                        help="ef_search for local nn20db query (default: 100)")
    args = parser.parse_args()

    # parse ESP32 address
    esp32_addr = None
    if args.esp32:
        parts = args.esp32.rsplit(":", 1)
        if len(parts) != 2:
            parser.error("--esp32 must be HOST:PORT")
        esp32_addr = (parts[0], int(parts[1]))

    # load word vectors into RAM
    word_vecs = load_word_vectors(args.word_vectors, limit=args.limit)

    # open nn20db (local search)
    db = None
    if _HAVE_NN20DB:
        print(f"Opening nn20db at '{args.db}' ...")
        try:
            db = NN20Db.open(args.db)
            print("  nn20db opened OK")
        except Exception as e:
            print(f"  Warning: could not open nn20db: {e}")
            print("  Local search will be unavailable.")
    else:
        print("Warning: nn20db Python API not found. "
              "Local search unavailable.")
        if esp32_addr is None:
            print("No search backend available. "
                  "Provide --esp32 or install the nn20db Python API.")
            sys.exit(1)

    try:
        repl(db, word_vecs, esp32_addr, k=args.k, ef=args.ef)
    finally:
        if db is not None:
            db.close()


if __name__ == "__main__":
    main()
