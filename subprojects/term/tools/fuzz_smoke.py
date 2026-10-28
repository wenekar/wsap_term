#!/usr/bin/env python3
"""Run a bounded fuzz check without changing the source corpus."""

import argparse
import shutil
import subprocess
import tempfile
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("fuzzer", type=Path)
    parser.add_argument("assets", type=Path)
    parser.add_argument("--artifacts", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=0,
                        help="fuzz for this time with a random seed, not 1000 fixed runs")
    args = parser.parse_args()
    args.artifacts.mkdir(parents=True, exist_ok=True)
    limit = ([f"-max_total_time={args.seconds}"] if args.seconds
             else ["-runs=1000", "-seed=1"])
    with tempfile.TemporaryDirectory(prefix="vtcore-fuzz-") as work:
        corpus = Path(work) / "corpus"
        shutil.copytree(args.assets / "corpus", corpus)
        return subprocess.run([
            str(args.fuzzer.resolve()), *limit, "-max_len=4096", "-print_final_stats=1",
            f"-dict={args.assets.resolve() / 'terminal.dict'}", str(corpus),
            f"-artifact_prefix={args.artifacts.resolve()}/",
        ], check=False).returncode


if __name__ == "__main__":
    raise SystemExit(main())
