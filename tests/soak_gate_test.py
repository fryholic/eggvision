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
        metrics = [
            {
                "type": "metrics",
                "capture_fps": 30.0,
                "rtsp_fps": 30.0,
                "inference_fps": 10.0,
            }
            for _ in range(max(30, math.ceil(duration / 5) + 12))
        ]
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
    if run_case(verifier).returncode != 0:
        raise RuntimeError("valid soak fixture was rejected")

    def slow_inference(metrics, _resources, _extra):
        metrics[-1]["inference_fps"] = 1.0

    def stopped_rtsp(metrics, _resources, _extra):
        metrics[-1]["rtsp_fps"] = 0.0

    def current_throttle(_metrics, resources, _extra):
        resources[-1] = resources[-1].replace("0xe0000", "0xe0001")

    def malformed_json(_metrics, _resources, extra):
        extra.append('{"type":"metrics","capture_fps":}')

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
        run_case(verifier, slow_inference),
        run_case(verifier, stopped_rtsp),
        run_case(verifier, current_throttle),
        run_case(verifier, malformed_json),
        run_case(verifier, truncated_resources, duration=1800),
        run_case(verifier, missing_rss),
        run_case(verifier, missing_fd),
        run_case(verifier, missing_temperature),
        run_case(verifier, compressed_coverage),
        run_case(verifier, runaway_rss),
        run_case(verifier, runaway_fd),
        run_case(verifier, rss_ceiling),
        run_case(verifier, fd_ceiling),
        run_case(verifier, after="throttled=0xf0000"),
    ]
    if any(result.returncode == 0 for result in failing):
        raise RuntimeError("invalid soak fixture passed")
    print("soak gate tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
