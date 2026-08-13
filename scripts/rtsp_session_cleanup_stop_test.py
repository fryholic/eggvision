#!/usr/bin/env python3
"""Ensure a slow cleanup owner cannot delay application stop."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import tempfile
import time
from pathlib import Path

from rtsp_error_recovery_test import stop_process, wait_for_listener
from rtsp_recovery_stop_race_test import wait_for_log
from rtsp_session_cleanup_test import setup_only


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, default=Path("./build/bsaps_app"))
    parser.add_argument("--url", default="rtsp://127.0.0.1:8554/stream")
    parser.add_argument("--iterations", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    app = args.app.resolve()
    if not app.is_file() or args.iterations < 1:
        parser.error("valid app and positive iterations are required")
    critical_pattern = re.compile(
        r"(?:GLib|GObject|GStreamer)[^\n]*CRITICAL|assertion [^\n]* failed|Segmentation fault",
        re.IGNORECASE,
    )
    for iteration in range(1, args.iterations + 1):
        with tempfile.TemporaryDirectory(prefix="bsaps-rtsp-cleanup-stop-") as directory:
            log_path = Path(directory) / "server.log"
            environment = os.environ.copy()
            environment["BSAPS_RTSP_TEST_SESSION_TIMEOUT_SECONDS"] = "1"
            environment["BSAPS_RTSP_TEST_SESSION_CLEANUP_DELAY_MS"] = "5000"
            environment["G_DEBUG"] = "fatal-criticals"
            with log_path.open("wb") as log_file:
                process = subprocess.Popen(
                    [str(app), "--no-inference", "--duration", "30"],
                    stdout=log_file,
                    stderr=subprocess.STDOUT,
                    env=environment,
                )
            try:
                wait_for_listener(args.url, process, args.timeout)
                if setup_only(args.url, args.timeout) != 200:
                    raise RuntimeError("SETUP did not create the cleanup test session")
                wait_for_log(
                    log_path,
                    "test hook delaying session cleanup by 5000ms",
                    process,
                    args.timeout,
                )
                started = time.monotonic()
                process.terminate()
                code = process.wait(timeout=3.0)
                elapsed = time.monotonic() - started
            finally:
                stop_process(process)
            log = log_path.read_text(encoding="utf-8", errors="replace")
            final = re.search(r"\[app\] stopped .*\boutstanding=(\d+)\b", log)
            if code != 0 or elapsed > 1.5 or not final or int(final.group(1)) != 0:
                raise RuntimeError(
                    f"iteration {iteration}: code={code} elapsed={elapsed:.2f}s "
                    f"outstanding={final.group(1) if final else 'missing'}\n" + log
                )
            critical = critical_pattern.search(log)
            if critical:
                raise RuntimeError(f"iteration {iteration}: {critical.group(0)}")
            print(
                f"cleanup-stop iteration={iteration} elapsed={elapsed:.3f}s passed",
                flush=True,
            )
    print(f"RTSP cleanup/stop isolation test passed cases={args.iterations}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
