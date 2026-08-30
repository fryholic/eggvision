#!/usr/bin/env python3
import json
import math
import subprocess
import sys
import tempfile
from datetime import datetime, timedelta, timezone
from pathlib import Path


def run_case(
    verifier,
    mutate=None,
    before="throttled=0xe0000",
    after="throttled=0xe0000",
    duration=120,
):
    with tempfile.TemporaryDirectory(prefix="eggvision-soak-gate-") as directory:
        root = Path(directory)
        metrics = []
        for uptime_seconds in range(5, duration, 5):
            metrics.append(
                {
                    "type": "metrics",
                    "uptime_ms": uptime_seconds * 1000,
                    "capture_fps": 30.0,
                    "rtsp_fps": 30.0,
                    "inference_fps": 10.0,
                }
            )
        started = datetime(2026, 8, 30, tzinfo=timezone(timedelta(hours=9)))
        resources = []
        for index in range(max(4, math.ceil(duration / 30))):
            timestamp = (started + timedelta(seconds=30 * index)).isoformat()
            resources.append(
                f"{timestamp} rss_kb=70000 fd_count=60 "
                "temp=55.0'C throttled=0xe0000"
            )
        extra_lines = []
        if mutate:
            mutate(metrics, resources, extra_lines)
        app_log = root / "app.log"
        resource_log = root / "resources.log"
        app_log.write_text(
            "\n".join(json.dumps(row, separators=(",", ":")) for row in metrics)
            + "\n" + "\n".join(extra_lines) + "\n"
        )
        resource_log.write_text("\n".join(resources) + "\n")
        return subprocess.run(
            [
                sys.executable,
                verifier,
                str(app_log),
                str(resource_log),
                str(duration),
                before,
                after,
            ],
            capture_output=True,
            text=True,
            check=False,
        )


def main():
    verifier = sys.argv[1]
    valid = run_case(verifier)
    if valid.returncode != 0:
        raise RuntimeError("valid soak fixture was rejected")
    valid_result = json.loads(valid.stdout)
    if (
        valid_result["metrics_coverage_seconds"] != 55.0
        or valid_result["metrics_max_gap_seconds"] != 5.0
        or valid_result["resource_coverage_seconds"] != 90.0
    ):
        raise RuntimeError(f"valid soak fixture summary is incorrect: {valid_result}")

    def slow_inference(metrics, _resources, _extra):
        metrics[-1]["inference_fps"] = 1.0

    def stopped_rtsp(metrics, _resources, _extra):
        metrics[-1]["rtsp_fps"] = 0.0

    def current_throttle(_metrics, resources, _extra):
        resources[-1] = resources[-1].replace("0xe0000", "0xe0001")

    def malformed_json(_metrics, _resources, extra):
        extra.append('{"type":"metrics","capture_fps":}')

    def prefixed_malformed_json(_metrics, _resources, extra):
        extra.append('prefix-corruption:{"type":"metrics","capture_fps":}')

    def missing_uptime(metrics, _resources, _extra):
        del metrics[-1]["uptime_ms"]

    def metrics_gap(metrics, _resources, _extra):
        metrics[:] = [
            row
            for row in metrics
            if not 600000 <= row["uptime_ms"] <= 945000
        ]

    def metrics_ended_early(metrics, _resources, _extra):
        metrics[:] = [row for row in metrics if row["uptime_ms"] <= 1450000]

    def non_finite_metric(metrics, _resources, _extra):
        metrics[-1]["inference_fps"] = float("nan")

    def truncated_resources(_metrics, resources, _extra):
        del resources[2:]

    def missing_rss(_metrics, resources, _extra):
        resources[-1] = resources[-1].replace(" rss_kb=70000", "")

    def missing_fd(_metrics, resources, _extra):
        resources[-1] = resources[-1].replace(" fd_count=60", "")

    def missing_temperature(_metrics, resources, _extra):
        resources[-1] = resources[-1].replace(" temp=55.0'C", "")

    def compressed_coverage(_metrics, resources, _extra):
        started = datetime(2026, 8, 30, tzinfo=timezone(timedelta(hours=9)))
        for index, line in enumerate(resources):
            timestamp, fields = line.split(" ", 1)
            del timestamp
            resources[index] = (
                f"{(started + timedelta(seconds=index)).isoformat()} {fields}"
            )

    def runaway_rss(_metrics, resources, _extra):
        resources[-1] = resources[-1].replace("rss_kb=70000", "rss_kb=100000")

    def runaway_fd(_metrics, resources, _extra):
        resources[-1] = resources[-1].replace("fd_count=60", "fd_count=100")

    def rss_ceiling(_metrics, resources, _extra):
        resources[-1] = resources[-1].replace("rss_kb=70000", "rss_kb=600000")

    def fd_ceiling(_metrics, resources, _extra):
        resources[-1] = resources[-1].replace("fd_count=60", "fd_count=2000")

    failing = [
        ("slow inference", run_case(verifier, slow_inference), "inference_fps fell below"),
        ("stopped RTSP", run_case(verifier, stopped_rtsp), "rtsp_fps fell below"),
        ("current throttle", run_case(verifier, current_throttle), "current throttling"),
        ("malformed JSON", run_case(verifier, malformed_json), "malformed JSON records"),
        (
            "prefixed malformed JSON",
            run_case(verifier, prefixed_malformed_json),
            "structured marker found outside a JSON record",
        ),
        ("missing uptime", run_case(verifier, missing_uptime), "invalid uptime_ms"),
        (
            "metrics gap",
            run_case(verifier, metrics_gap, duration=1800),
            "invalid metrics uptime gap",
        ),
        (
            "metrics ended early",
            run_case(verifier, metrics_ended_early, duration=1800),
            "metrics ended early",
        ),
        (
            "non-finite metric",
            run_case(verifier, non_finite_metric),
            "non-finite inference_fps",
        ),
        (
            "truncated resources",
            run_case(verifier, truncated_resources, duration=1800),
            "insufficient resource samples",
        ),
        ("missing RSS", run_case(verifier, missing_rss), "malformed resource sample"),
        ("missing FD", run_case(verifier, missing_fd), "malformed resource sample"),
        (
            "missing temperature",
            run_case(verifier, missing_temperature),
            "malformed resource sample",
        ),
        (
            "compressed resource coverage",
            run_case(verifier, compressed_coverage),
            "insufficient resource time coverage",
        ),
        ("RSS growth", run_case(verifier, runaway_rss), "RSS growth exceeded"),
        ("FD growth", run_case(verifier, runaway_fd), "FD growth exceeded"),
        ("RSS ceiling", run_case(verifier, rss_ceiling), "RSS ceiling exceeded"),
        ("FD ceiling", run_case(verifier, fd_ceiling), "FD ceiling exceeded"),
        (
            "new throttle history",
            run_case(verifier, after="throttled=0xf0000"),
            "new throttling history bits appeared",
        ),
    ]
    for name, result, expected_error in failing:
        if result.returncode == 0:
            raise RuntimeError(f"invalid soak fixture passed: {name}")
        if expected_error not in result.stderr:
            raise RuntimeError(
                f"invalid soak fixture failed for the wrong reason: {name}: {result.stderr}"
            )
    print("soak gate tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
