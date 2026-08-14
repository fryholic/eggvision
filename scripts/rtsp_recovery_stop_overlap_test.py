#!/usr/bin/env python3
"""Verify delayed recovery stays owned through stop and a clean lease release."""

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
from rtsp_recovery_stop_race_test import wait_for_log


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, default=Path("./build/eggvision_app"))
    parser.add_argument("--url", default="rtsp://127.0.0.1:8554/stream")
    parser.add_argument("--iterations", type=int, default=1)
    parser.add_argument("--duration", type=int, default=30)
    parser.add_argument("--teardown-delay-ms", type=int, default=20000)
    parser.add_argument("--warning-seconds", type=float, default=15.0)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    app = args.app.resolve()
    if (
        not app.is_file()
        or args.iterations < 1
        or args.duration < 2
        or args.teardown_delay_ms <= args.warning_seconds * 1000
    ):
        parser.error(
            "valid app, positive iterations, duration >= 2, and a teardown delay "
            "beyond the warning boundary are required"
        )

    critical_pattern = re.compile(
        r"(?:GLib|GObject|GStreamer)[^\n]*CRITICAL|assertion [^\n]* failed|Segmentation fault",
        re.IGNORECASE,
    )
    for iteration in range(1, args.iterations + 1):
        with tempfile.TemporaryDirectory(prefix="eggvision-rtsp-stop-overlap-") as directory:
            root = Path(directory)
            trigger = root / "inject-error"
            log_path = root / "server.log"
            environment = os.environ.copy()
            environment["EGGVISION_RTSP_TEST_PUSH_ERROR_TRIGGER"] = str(trigger)
            environment["EGGVISION_RTSP_TEST_TEARDOWN_DELAY_MS"] = str(args.teardown_delay_ms)
            environment["G_DEBUG"] = "fatal-criticals"
            with log_path.open("wb") as log_file:
                process = subprocess.Popen(
                    [str(app), "--no-inference", "--duration", str(args.duration)],
                    stdout=log_file,
                    stderr=subprocess.STDOUT,
                    env=environment,
                )
            code = -1
            stop_started = time.monotonic()
            try:
                wait_for_listener(args.url, process, args.timeout)
                connection = RtspConnection(args.url, args.timeout)
                try:
                    connection.start()
                    trigger.touch()
                    wait_for_log(
                        log_path,
                        "recovery teardown started:",
                        process,
                        args.timeout,
                    )
                    stop_started = time.monotonic()
                    process.terminate()

                    # The old policy returned at 15 seconds, detaching a job
                    # that could still own a FrameLease. Stop must keep the
                    # process and every transitive owner alive at that boundary.
                    early_deadline = stop_started + args.warning_seconds - 0.5
                    while time.monotonic() < early_deadline:
                        if process.poll() is not None:
                            raise RuntimeError(
                                f"iteration {iteration}: process exited before the "
                                "recovery warning boundary"
                            )
                        time.sleep(0.1)
                    wait_for_log(
                        log_path,
                        "shutdown delayed: recovery teardown token=",
                        process,
                        2.0,
                    )
                    code = process.wait(
                        timeout=args.teardown_delay_ms / 1000 - args.warning_seconds + 5
                    )
                finally:
                    connection.close()
            finally:
                stop_process(process)

            elapsed = time.monotonic() - stop_started
            log = log_path.read_text(encoding="utf-8", errors="replace")
            if code != 0:
                raise RuntimeError(f"iteration {iteration}: exit code {code}")
            minimum_wait = args.teardown_delay_ms / 1000 - 0.75
            maximum_wait = args.teardown_delay_ms / 1000 + 3.0
            if elapsed < minimum_wait or elapsed > maximum_wait:
                raise RuntimeError(
                    f"iteration {iteration}: owned recovery wait was {elapsed:.2f}s, "
                    f"expected {minimum_wait:.2f}s to {maximum_wait:.2f}s"
                )
            final = re.search(r"\[app\] stopped .*\boutstanding=(\d+)\b", log)
            if not final or int(final.group(1)) != 0:
                raise RuntimeError(f"iteration {iteration}: final outstanding was not zero")
            for evidence in (
                "shutdown delayed: recovery teardown token=",
                "retaining owners and waiting safely",
                "delayed recovery teardown completed token=",
            ):
                if evidence not in log:
                    raise RuntimeError(
                        f"iteration {iteration}: missing shutdown evidence: {evidence}"
                    )
            capture_stopped = log.find("[camera] capture stopped")
            delayed = log.find("shutdown delayed: recovery teardown token=")
            completed = log.find("delayed recovery teardown completed token=")
            app_stopped = log.find("[app] stopped")
            if not 0 <= capture_stopped < delayed < completed < app_stopped:
                raise RuntimeError(
                    f"iteration {iteration}: shutdown/lease-release evidence is out of order"
                )
            critical = critical_pattern.search(log)
            if critical:
                raise RuntimeError(f"iteration {iteration}: {critical.group(0)}")
            print(
                f"stop-overlap iteration={iteration} elapsed={elapsed:.2f}s "
                "exit=0 outstanding=0 passed",
                flush=True,
            )

    print(
        f"RTSP delayed recovery shutdown test passed cases={args.iterations} "
        f"warning={args.warning_seconds:.0f}s delay={args.teardown_delay_ms}ms",
        flush=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
