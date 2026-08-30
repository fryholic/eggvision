#!/usr/bin/env python3
import json
import subprocess
import sys
import tempfile
from pathlib import Path


def run_case(verifier, mutate=None, before="throttled=0xe0000", after="throttled=0xe0000"):
    with tempfile.TemporaryDirectory(prefix="eggvision-soak-gate-") as directory:
        root = Path(directory)
        metrics = [
            {
                "type": "metrics",
                "capture_fps": 30.0,
                "rtsp_fps": 30.0,
                "inference_fps": 10.0,
            }
            for _ in range(30)
        ]
        resources = [
            f"2026-08-30T00:00:{index:02d}+09:00 rss_kb=70000 fd_count=60 "
            "temp=55.0'C throttled=0xe0000"
            for index in range(4)
        ]
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
            [sys.executable, verifier, str(app_log), str(resource_log), "120", before, after],
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

    failing = [
        run_case(verifier, slow_inference),
        run_case(verifier, stopped_rtsp),
        run_case(verifier, current_throttle),
        run_case(verifier, malformed_json),
        run_case(verifier, after="throttled=0xf0000"),
    ]
    if any(result.returncode == 0 for result in failing):
        raise RuntimeError("invalid soak fixture passed")
    print("soak gate tests passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
