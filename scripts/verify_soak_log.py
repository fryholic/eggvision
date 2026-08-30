#!/usr/bin/env python3
import json
import re
import sys
from pathlib import Path


def parse_throttled(value):
    match = re.fullmatch(r"throttled=0x([0-9a-fA-F]+)", value.strip())
    if not match:
        raise ValueError(f"invalid get_throttled value: {value}")
    return int(match.group(1), 16)


def verify(app_log, resource_log, duration, before_text, after_text):
    metrics = []
    malformed = []
    for line_number, line in enumerate(Path(app_log).read_text(errors="replace").splitlines(), 1):
        if not line.startswith("{"):
            continue
        try:
            row = json.loads(line)
        except json.JSONDecodeError as error:
            malformed.append({"line": line_number, "error": str(error)})
            continue
        if row.get("type") == "metrics":
            metrics.append(row)
    if malformed:
        raise RuntimeError(f"malformed JSON records: {malformed[:3]}")

    warmup_samples = 12
    measured = metrics[warmup_samples:]
    minimum_samples = max(1, int(max(0, duration - 60) / 5 * 0.8))
    if len(measured) < minimum_samples:
        raise RuntimeError(
            f"insufficient measured metrics: {len(measured)} < {minimum_samples}"
        )
    thresholds = {"capture_fps": 25.0, "rtsp_fps": 25.0, "inference_fps": 8.0}
    for field, threshold in thresholds.items():
        failures = [
            index + warmup_samples
            for index, row in enumerate(measured)
            if float(row.get(field, -1.0)) < threshold
        ]
        if failures:
            raise RuntimeError(
                f"{field} fell below {threshold} at metric indexes {failures[:10]}"
            )

    before = parse_throttled(before_text)
    after = parse_throttled(after_text)
    if before & 0xF or after & 0xF:
        raise RuntimeError(
            f"current throttling detected before=0x{before:x} after=0x{after:x}"
        )
    new_latched = (after & 0xF0000) & ~(before & 0xF0000)
    if new_latched:
        raise RuntimeError(f"new throttling history bits appeared: 0x{new_latched:x}")

    resource_samples = 0
    for line in Path(resource_log).read_text(errors="replace").splitlines():
        match = re.search(r"throttled=0x([0-9a-fA-F]+)", line)
        if not match:
            continue
        resource_samples += 1
        if int(match.group(1), 16) & 0xF:
            raise RuntimeError(f"current throttling observed in resource log: {line}")
    if resource_samples < 2:
        raise RuntimeError(f"insufficient throttle resource samples: {resource_samples}")

    return {
        "metrics": len(metrics),
        "measured_metrics": len(measured),
        "resource_samples": resource_samples,
        "minimum_capture_fps": min(float(row["capture_fps"]) for row in measured),
        "minimum_rtsp_fps": min(float(row["rtsp_fps"]) for row in measured),
        "minimum_inference_fps": min(float(row["inference_fps"]) for row in measured),
        "before_throttled": before_text,
        "after_throttled": after_text,
    }


def main():
    if len(sys.argv) != 6:
        print(
            "usage: verify_soak_log.py APP_LOG RESOURCE_LOG DURATION BEFORE_THROTTLED "
            "AFTER_THROTTLED",
            file=sys.stderr,
        )
        return 2
    try:
        result = verify(sys.argv[1], sys.argv[2], int(sys.argv[3]), sys.argv[4], sys.argv[5])
        print(json.dumps(result, sort_keys=True))
        return 0
    except (OSError, ValueError, RuntimeError) as error:
        print(f"soak log verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
