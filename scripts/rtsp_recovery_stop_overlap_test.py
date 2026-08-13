#!/usr/bin/env python3
"""Overlap delayed RTSP recovery with application stop repeatedly."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path

from rtsp_error_recovery_test import stop_process, wait_for_listener
from rtsp_lifecycle_test import RtspConnection


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, default=Path("./build/bsaps_app"))
    parser.add_argument("--url", default="rtsp://127.0.0.1:8554/stream")
    parser.add_argument("--iterations", type=int, default=5)
    parser.add_argument("--duration", type=int, default=2)
    parser.add_argument("--teardown-delay-ms", type=int, default=7000)
    parser.add_argument("--timeout", type=float, default=8.0)
    args = parser.parse_args()
    app = args.app.resolve()
    if not app.is_file() or args.iterations < 1 or args.duration < 2:
        parser.error("valid app, positive iterations, and duration >= 2 are required")

    critical_pattern = re.compile(
        r"(?:GLib|GObject|GStreamer)[^\n]*CRITICAL|assertion [^\n]* failed|Segmentation fault",
        re.IGNORECASE,
    )
    for iteration in range(1, args.iterations + 1):
        with tempfile.TemporaryDirectory(prefix="bsaps-rtsp-stop-overlap-") as directory:
            root = Path(directory)
            trigger = root / "inject-error"
            log_path = root / "server.log"
            environment = os.environ.copy()
            environment["BSAPS_RTSP_TEST_PUSH_ERROR_TRIGGER"] = str(trigger)
            environment["BSAPS_RTSP_TEST_TEARDOWN_DELAY_MS"] = str(args.teardown_delay_ms)
            environment["G_DEBUG"] = "fatal-criticals"
            with log_path.open("wb") as log_file:
                process = subprocess.Popen(
                    [str(app), "--no-inference", "--duration", str(args.duration)],
                    stdout=log_file,
                    stderr=subprocess.STDOUT,
                    env=environment,
                )
            started = time.monotonic()
            try:
                wait_for_listener(args.url, process, args.timeout)
                connection = RtspConnection(args.url, args.timeout)
                try:
                    connection.start()
                    trigger.touch()
                    process.wait(
                        timeout=args.duration + args.teardown_delay_ms / 1000 + 5
                    )
                finally:
                    connection.close()
            finally:
                stop_process(process)
            elapsed = time.monotonic() - started
            log = log_path.read_text(encoding="utf-8", errors="replace")
            if process.returncode != 0:
                raise RuntimeError(f"iteration {iteration}: exit code {process.returncode}")
            minimum_wait = args.teardown_delay_ms / 1000
            if elapsed < minimum_wait or elapsed > minimum_wait + 4:
                raise RuntimeError(
                    f"iteration {iteration}: bounded recovery wait was {elapsed:.2f}s, "
                    f"expected {minimum_wait:.2f}s to {minimum_wait + 4:.2f}s"
                )
            final = re.search(r"\[app\] stopped .*\boutstanding=(\d+)\b", log)
            if not final or int(final.group(1)) != 0:
                raise RuntimeError(f"iteration {iteration}: final outstanding was not zero")
            critical = critical_pattern.search(log)
            if critical:
                raise RuntimeError(f"iteration {iteration}: {critical.group(0)}")
            print(f"stop-overlap iteration={iteration} elapsed={elapsed:.2f}s passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
