
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

PACKET_SIZE_BYTES = 1500
PACKETS_PER_MBPS = 1_000_000 / (PACKET_SIZE_BYTES * 8)
MS_PER_SECOND = 1000

# Some files use "########...\\nRun N\\n", others use only "Run N\\n".
RUN_HEADER_RE = re.compile(
    r"(?:#{5,}\s*\n)?Run\s+(\d+)\s*\n",
    re.MULTILINE,
)


def parse_list_runs(text: str) -> list[tuple[int, list[float]]]:
    """Split a .list file into numbered runs."""
    text = text.strip()
    if not text:
        return []

    matches = list(RUN_HEADER_RE.finditer(text))
    if not matches:
        values = _parse_values(text)
        return [(1, values)] if values else []

    runs: list[tuple[int, list[float]]] = []
    for i, match in enumerate(matches):
        run_num = int(match.group(1))
        start = match.end()
        end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
        body = text[start:end]
        if "###############################" in body:
            body = body.split("###############################", 1)[0]
        values = _parse_values(body)
        if values:
            runs.append((run_num, values))
    return runs


def _parse_values(block: str) -> list[float]:
    block = block.strip()
    if not block:
        return []
    # Values may wrap across lines; treat line breaks as comma boundaries.
    block = re.sub(r"\s*\n\s*", ",", block)
    return [float(x.strip()) for x in block.split(",") if x.strip()]


def throughput_to_trace(throughputs_mbps: list[float]) -> list[int]:
    """Mbps time series (1 Hz) -> Mahimahi packet delivery timestamps (ms)."""
    timestamps: list[int] = []
    for sec, mbps in enumerate(throughputs_mbps):
        count = int(mbps * PACKETS_PER_MBPS)
        if count <= 0:
            continue
        base_ms = sec * MS_PER_SECOND
        for i in range(count):
            timestamps.append(base_ms + int(i * MS_PER_SECOND / count))
    return timestamps


def write_trace(path: Path, timestamps: list[int]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        for ts in timestamps:
            f.write(f"{ts}\n")


def write_latency(path: Path, samples_ms: list[float]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", encoding="utf-8", newline="\n") as f:
        for value in samples_ms:
            f.write(f"{value}\n")


def convert_sigcomm_tree(
    input_root: Path,
    output_root: Path,
    kinds: set[str],
) -> tuple[int, int]:
    """Return (files_written, runs_written)."""
    files_written = 0
    runs_written = 0

    for kind in sorted(kinds):
        kind_root = input_root / kind
        if not kind_root.is_dir():
            print(f"warning: missing directory {kind_root}", file=sys.stderr)
            continue

        for src in sorted(kind_root.rglob("*.list")):
            rel = src.relative_to(kind_root)
            out_dir = output_root / kind / rel.parent / rel.stem
            runs = parse_list_runs(src.read_text(encoding="utf-8", errors="replace"))

            for run_num, values in runs:
                if kind == "throughput":
                    trace = throughput_to_trace(values)
                    if not trace:
                        print(
                            f"warning: empty trace for {src} run {run_num}",
                            file=sys.stderr,
                        )
                        continue
                    out_path = out_dir / f"run{run_num}.trace"
                    write_trace(out_path, trace)
                elif kind == "ping":
                    out_path = out_dir / f"run{run_num}.latency"
                    write_latency(out_path, values)
                else:
                    raise ValueError(f"unknown kind: {kind}")

                files_written += 1
                runs_written += 1

            print(f"{src.relative_to(input_root)} -> {len(runs)} run(s)")

    return files_written, runs_written


def default_sigcomm_root() -> Path:
    here = Path(__file__).resolve().parent
    candidates = [
        here / "extracted" / "sigcomm" / "sigcomm-5gmemu-5g-mmWave-uplink-data",
        here / "sigcomm-5gmemu-5g-mmWave-uplink-data",
    ]
    for path in candidates:
        if path.is_dir():
            return path
    return candidates[0]


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert SIGCOMM .list files to Mahimahi trace format."
    )
    parser.add_argument(
        "--input",
        type=Path,
        default=default_sigcomm_root(),
        help="Root of extracted sigcomm-5gmemu-5g-mmWave-uplink-data",
    )
    parser.add_argument(
        "--output",
        type=Path,
        default=None,
        help="Output directory (default: <input>/mahimahi-traces)",
    )
    parser.add_argument(
        "--throughput-only",
        action="store_true",
        help="Convert throughput/*.list only (skip ping latency export)",
    )
    parser.add_argument(
        "--ping-only",
        action="store_true",
        help="Convert ping/*.list only",
    )
    args = parser.parse_args()

    input_root = args.input.resolve()
    if not input_root.is_dir():
        print(f"error: input directory not found: {input_root}", file=sys.stderr)
        return 1

    output_root = (
        args.output.resolve()
        if args.output
        else input_root / "mahimahi-traces"
    )

    if args.throughput_only:
        kinds = {"throughput"}
    elif args.ping_only:
        kinds = {"ping"}
    else:
        kinds = {"throughput", "ping"}

    print(f"input : {input_root}")
    print(f"output: {output_root}")
    files_written, runs_written = convert_sigcomm_tree(input_root, output_root, kinds)
    print(f"done: {files_written} files, {runs_written} runs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
