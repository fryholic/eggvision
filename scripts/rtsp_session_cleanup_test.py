#!/usr/bin/env python3
"""Verify bounded RTSP sessions and cleanup on its dedicated context."""

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


def setup_only(url: str, timeout: float) -> int:
    connection = RtspConnection(url, timeout)
    try:
        connection.require(connection.request("OPTIONS"), {200}, "OPTIONS")
        describe = connection.request("DESCRIBE", headers={"Accept": "application/sdp"})
        connection.require(describe, {200}, "DESCRIBE")
        setup = connection.request(
            "SETUP",
            connection._control_url(describe),  # test helper intentionally reuses parser
            {"Transport": "RTP/AVP/TCP;unicast;interleaved=0-1"},
        )
        return setup.code
    finally:
        connection.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--app", type=Path, default=Path("./build/bsaps_app"))
    parser.add_argument("--url", default="rtsp://127.0.0.1:8554/stream")
    parser.add_argument("--max-sessions", type=int, default=8)
    parser.add_argument("--timeout", type=float, default=10.0)
    args = parser.parse_args()
    app = args.app.resolve()
    if not app.is_file() or args.max_sessions < 2 or args.timeout <= 0:
        parser.error("valid app, max-sessions >= 2, and timeout are required")

    with tempfile.TemporaryDirectory(prefix="bsaps-rtsp-session-cleanup-") as directory:
        root = Path(directory)
        log_path = root / "server.log"
        environment = os.environ.copy()
        environment["BSAPS_RTSP_TEST_SESSION_TIMEOUT_SECONDS"] = "4"
        environment["BSAPS_RTSP_TEST_SESSION_CLEANUP_DELAY_MS"] = "2000"
        environment["G_DEBUG"] = "fatal-criticals"
        with log_path.open("wb") as log_file:
            process = subprocess.Popen(
                [
                    str(app), "--no-inference", "--duration", "30",
                    "--max-rtsp-sessions", str(args.max_sessions),
                ],
                stdout=log_file,
                stderr=subprocess.STDOUT,
                env=environment,
            )
        try:
            wait_for_listener(args.url, process, args.timeout)
            for index in range(args.max_sessions):
                code = setup_only(args.url, args.timeout)
                if code != 200:
                    raise RuntimeError(f"session {index + 1} SETUP returned {code}")
            rejected = setup_only(args.url, args.timeout)
            if rejected != 503:
                raise RuntimeError(f"session cap probe returned {rejected}, expected 503")

            try:
                wait_for_log(
                    log_path,
                    "test hook delaying session cleanup by 2000ms",
                    process,
                    args.timeout,
                )
            except Exception:
                print(log_path.read_text(encoding="utf-8", errors="replace"), flush=True)
                raise
            started = time.monotonic()
            probe = RtspConnection(args.url, args.timeout)
            try:
                probe.require(probe.request("OPTIONS"), {200}, "OPTIONS during cleanup")
            finally:
                probe.close()
            listener_latency = time.monotonic() - started
            if listener_latency > 0.75:
                raise RuntimeError(
                    f"listener was blocked by cleanup for {listener_latency:.2f}s"
                )

            wait_for_log(log_path, "session cleanup removed=", process, args.timeout)
            fresh = RtspConnection(args.url, args.timeout)
            try:
                fresh.start()
                fresh.teardown()
            finally:
                fresh.close()
            process.terminate()
            code = process.wait(timeout=args.timeout)
        finally:
            stop_process(process)

        log = log_path.read_text(encoding="utf-8", errors="replace")
        failures: list[str] = []
        final = re.search(
            r"\[app\] stopped .*\boutstanding=(\d+)\b.*"
            r"\brtsp_sessions_current=(\d+)\b.*\brtsp_sessions_peak=(\d+)\b.*"
            r"\brtsp_sessions_cleaned=(\d+)\b",
            log,
        )
        if code != 0:
            failures.append(f"exit code was {code}, expected 0")
        if not final:
            failures.append("final session metrics are missing")
        else:
            outstanding, current, peak, cleaned = map(int, final.groups())
            if outstanding != 0:
                failures.append(f"outstanding was {outstanding}, expected 0")
            if current > args.max_sessions or peak > args.max_sessions:
                failures.append(
                    f"session pool exceeded cap: current={current} peak={peak}"
                )
            if cleaned < args.max_sessions:
                failures.append(
                    f"cleaned sessions was {cleaned}, expected at least {args.max_sessions}"
                )
        critical = re.search(
            r"(?:GLib|GObject|GStreamer)[^\n]*CRITICAL|assertion [^\n]* failed|Segmentation fault",
            log,
            re.IGNORECASE,
        )
        if critical:
            failures.append(f"critical runtime diagnostic: {critical.group(0)}")
        if failures:
            raise RuntimeError(
                "; ".join(failures) + "\n--- log tail ---\n"
                + "\n".join(log.splitlines()[-100:])
            )
        print(
            "RTSP session cleanup test passed "
            f"max={args.max_sessions} listener_latency={listener_latency:.3f}s "
            f"cleaned={cleaned} outstanding=0",
            flush=True,
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
