#!/usr/bin/env python3
import json
import math
import re
import statistics
import sys
from datetime import datetime
from pathlib import Path


RESOURCE_INTERVAL_SECONDS = 30
RESOURCE_MIN_SAMPLE_RATIO = 0.8
RESOURCE_MAX_GAP_SECONDS = 65
RESOURCE_MAX_RSS_KB = 512 * 1024
RESOURCE_MAX_FD_COUNT = 1024
RESOURCE_RSS_GROWTH_FLOOR_KB = 16 * 1024
RESOURCE_RSS_GROWTH_RATIO = 0.20
RESOURCE_FD_GROWTH_FLOOR = 16
RESOURCE_FD_GROWTH_RATIO = 0.25
METRICS_INTERVAL_SECONDS = 5
METRICS_WARMUP_SECONDS = 60
METRICS_MIN_SAMPLE_RATIO = 0.8
METRICS_MAX_GAP_SECONDS = 7.5
STRUCTURED_LOG_MARKERS = ('"type":',)
RESOURCE_LINE = re.compile(
    r"(?P<timestamp>\S+) rss_kb=(?P<rss_kb>\d+) fd_count=(?P<fd_count>\d+) "
    r"temp=(?P<temperature>-?(?:\d+(?:\.\d*)?|\.\d+))'C "
    r"throttled=0x(?P<throttled>[0-9a-fA-F]+)"
)


def parse_throttled(value):
    match = re.fullmatch(r"throttled=0x([0-9a-fA-F]+)", value.strip())
    if not match:
        raise ValueError(f"invalid get_throttled value: {value}")
    return int(match.group(1), 16)


def parse_resource_samples(resource_log):
    samples = []
    for line_number, line in enumerate(
        Path(resource_log).read_text(errors="replace").splitlines(), 1
    ):
        if not line.strip():
            continue
        match = RESOURCE_LINE.fullmatch(line.strip())
        if not match:
            raise RuntimeError(f"malformed resource sample at line {line_number}: {line}")
        try:
            timestamp = datetime.fromisoformat(match.group("timestamp"))
        except ValueError as error:
            raise RuntimeError(
                f"invalid resource timestamp at line {line_number}: {line}"
            ) from error
        if timestamp.utcoffset() is None:
            raise RuntimeError(
                f"resource timestamp lacks timezone at line {line_number}: {line}"
            )
        rss_kb = int(match.group("rss_kb"))
        fd_count = int(match.group("fd_count"))
        temperature = float(match.group("temperature"))
        throttled = int(match.group("throttled"), 16)
        if rss_kb <= 0 or fd_count <= 0:
            raise RuntimeError(f"invalid resource value at line {line_number}: {line}")
        if not math.isfinite(temperature) or not -40.0 <= temperature <= 100.0:
            raise RuntimeError(f"invalid resource temperature at line {line_number}: {line}")
        samples.append(
            {
                "timestamp": timestamp,
                "rss_kb": rss_kb,
                "fd_count": fd_count,
                "temperature": temperature,
                "throttled": throttled,
                "line": line,
            }
        )
    return samples


def verify_resource_stability(resource_log, duration):
    samples = parse_resource_samples(resource_log)
    minimum_samples = max(
        2,
        math.floor(duration / RESOURCE_INTERVAL_SECONDS * RESOURCE_MIN_SAMPLE_RATIO),
    )
    if len(samples) < minimum_samples:
        raise RuntimeError(
            f"insufficient resource samples: {len(samples)} < {minimum_samples}"
        )

    for previous, current in zip(samples, samples[1:]):
        gap = (current["timestamp"] - previous["timestamp"]).total_seconds()
        if gap <= 0 or gap > RESOURCE_MAX_GAP_SECONDS:
            raise RuntimeError(f"invalid resource sample gap: {gap:.1f}s")
    coverage = (samples[-1]["timestamp"] - samples[0]["timestamp"]).total_seconds()
    minimum_coverage = max(0, duration - 2 * RESOURCE_INTERVAL_SECONDS)
    if coverage < minimum_coverage:
        raise RuntimeError(
            f"insufficient resource time coverage: {coverage:.1f}s < {minimum_coverage}s"
        )

    for sample in samples:
        if sample["throttled"] & 0xF:
            raise RuntimeError(
                f"current throttling observed in resource log: {sample['line']}"
            )
        if sample["rss_kb"] > RESOURCE_MAX_RSS_KB:
            raise RuntimeError(
                f"RSS ceiling exceeded: {sample['rss_kb']} KiB > {RESOURCE_MAX_RSS_KB} KiB"
            )
        if sample["fd_count"] > RESOURCE_MAX_FD_COUNT:
            raise RuntimeError(
                f"FD ceiling exceeded: {sample['fd_count']} > {RESOURCE_MAX_FD_COUNT}"
            )

    window_size = min(5, max(1, len(samples) // 4))
    baseline_start = min(4, max(0, len(samples) // 3))
    baseline = samples[baseline_start : baseline_start + window_size]
    tail = samples[-window_size:]
    baseline_rss = statistics.median(sample["rss_kb"] for sample in baseline)
    tail_rss = statistics.median(sample["rss_kb"] for sample in tail)
    tail_rss_max = max(sample["rss_kb"] for sample in tail)
    rss_growth_limit = max(
        RESOURCE_RSS_GROWTH_FLOOR_KB,
        baseline_rss * RESOURCE_RSS_GROWTH_RATIO,
    )
    if tail_rss_max - baseline_rss > rss_growth_limit:
        raise RuntimeError(
            "RSS growth exceeded: "
            f"baseline={baseline_rss:.1f} KiB tail_max={tail_rss_max} KiB "
            f"limit={rss_growth_limit:.1f} KiB"
        )

    baseline_fd = statistics.median(sample["fd_count"] for sample in baseline)
    tail_fd = statistics.median(sample["fd_count"] for sample in tail)
    tail_fd_max = max(sample["fd_count"] for sample in tail)
    fd_growth_limit = max(
        RESOURCE_FD_GROWTH_FLOOR,
        baseline_fd * RESOURCE_FD_GROWTH_RATIO,
    )
    if tail_fd_max - baseline_fd > fd_growth_limit:
        raise RuntimeError(
            "FD growth exceeded: "
            f"baseline={baseline_fd:.1f} tail_max={tail_fd_max} "
            f"limit={fd_growth_limit:.1f}"
        )

    return {
        "resource_samples": len(samples),
        "resource_coverage_seconds": coverage,
        "rss_baseline_kb": baseline_rss,
        "rss_tail_kb": tail_rss,
        "rss_tail_max_kb": tail_rss_max,
        "rss_final_kb": samples[-1]["rss_kb"],
        "rss_peak_kb": max(sample["rss_kb"] for sample in samples),
        "fd_baseline": baseline_fd,
        "fd_tail": tail_fd,
        "fd_tail_max": tail_fd_max,
        "fd_final": samples[-1]["fd_count"],
        "fd_peak": max(sample["fd_count"] for sample in samples),
        "minimum_temperature_c": min(sample["temperature"] for sample in samples),
        "maximum_temperature_c": max(sample["temperature"] for sample in samples),
    }


def parse_metrics(app_log):
    metrics = []
    malformed = []
    for line_number, line in enumerate(Path(app_log).read_text(errors="replace").splitlines(), 1):
        if not line.startswith("{"):
            if any(marker in line for marker in STRUCTURED_LOG_MARKERS):
                malformed.append(
                    {
                        "line": line_number,
                        "error": "structured marker found outside a JSON record",
                    }
                )
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
    return metrics


def metric_number(row, field, index):
    value = row.get(field)
    if isinstance(value, bool) or not isinstance(value, (int, float)):
        raise RuntimeError(f"invalid {field} at metric index {index}: {value!r}")
    number = float(value)
    if not math.isfinite(number):
        raise RuntimeError(f"non-finite {field} at metric index {index}: {value!r}")
    return number


def verify_metrics(app_log, duration):
    metrics = parse_metrics(app_log)
    if not metrics:
        raise RuntimeError("no metrics records")

    uptime_seconds = []
    threshold_fields = ("capture_fps", "rtsp_fps", "inference_fps")
    for index, row in enumerate(metrics):
        uptime_ms = metric_number(row, "uptime_ms", index)
        if uptime_ms < 0:
            raise RuntimeError(f"negative uptime_ms at metric index {index}: {uptime_ms}")
        uptime_seconds.append(uptime_ms / 1000.0)
        for field in threshold_fields:
            metric_number(row, field, index)

    gaps = [
        current - previous
        for previous, current in zip(uptime_seconds, uptime_seconds[1:])
    ]
    invalid_gaps = [gap for gap in gaps if gap <= 0 or gap > METRICS_MAX_GAP_SECONDS]
    if invalid_gaps:
        raise RuntimeError(f"invalid metrics uptime gap: {invalid_gaps[:10]}")

    measured = [
        (index, row, uptime)
        for index, (row, uptime) in enumerate(zip(metrics, uptime_seconds))
        if uptime >= METRICS_WARMUP_SECONDS
    ]
    minimum_samples = max(
        1,
        math.floor(
            max(0, duration - METRICS_WARMUP_SECONDS)
            / METRICS_INTERVAL_SECONDS
            * METRICS_MIN_SAMPLE_RATIO
        ),
    )
    if len(measured) < minimum_samples:
        raise RuntimeError(
            f"insufficient measured metrics: {len(measured)} < {minimum_samples}"
        )

    first_measured_uptime = measured[0][2]
    last_measured_uptime = measured[-1][2]
    if first_measured_uptime > METRICS_WARMUP_SECONDS + METRICS_MAX_GAP_SECONDS:
        raise RuntimeError(
            f"metrics started late after warm-up: {first_measured_uptime:.3f}s"
        )
    minimum_last_uptime = max(0, duration - METRICS_MAX_GAP_SECONDS)
    if last_measured_uptime < minimum_last_uptime:
        raise RuntimeError(
            "metrics ended early: "
            f"{last_measured_uptime:.3f}s < {minimum_last_uptime:.3f}s"
        )
    metrics_coverage = last_measured_uptime - first_measured_uptime
    minimum_coverage = max(
        0,
        duration - METRICS_WARMUP_SECONDS - 2 * METRICS_MAX_GAP_SECONDS,
    )
    if metrics_coverage < minimum_coverage:
        raise RuntimeError(
            f"insufficient metrics time coverage: {metrics_coverage:.3f}s "
            f"< {minimum_coverage:.3f}s"
        )

    thresholds = {"capture_fps": 25.0, "rtsp_fps": 25.0, "inference_fps": 8.0}
    for field, threshold in thresholds.items():
        failures = [
            index
            for index, row, _uptime in measured
            if metric_number(row, field, index) < threshold
        ]
        if failures:
            raise RuntimeError(
                f"{field} fell below {threshold} at metric indexes {failures[:10]}"
            )

    return {
        "metrics": len(metrics),
        "measured_metrics": len(measured),
        "metrics_first_measured_uptime_seconds": first_measured_uptime,
        "metrics_last_measured_uptime_seconds": last_measured_uptime,
        "metrics_coverage_seconds": metrics_coverage,
        "metrics_max_gap_seconds": max(gaps, default=0.0),
        "minimum_capture_fps": min(
            metric_number(row, "capture_fps", index) for index, row, _ in measured
        ),
        "minimum_rtsp_fps": min(
            metric_number(row, "rtsp_fps", index) for index, row, _ in measured
        ),
        "minimum_inference_fps": min(
            metric_number(row, "inference_fps", index) for index, row, _ in measured
        ),
    }


def verify(app_log, resource_log, duration, before_text, after_text):
    if duration <= 0:
        raise ValueError(f"duration must be positive: {duration}")
    metrics_result = verify_metrics(app_log, duration)

    before = parse_throttled(before_text)
    after = parse_throttled(after_text)
    if before & 0xF or after & 0xF:
        raise RuntimeError(
            f"current throttling detected before=0x{before:x} after=0x{after:x}"
        )
    new_latched = (after & 0xF0000) & ~(before & 0xF0000)
    if new_latched:
        raise RuntimeError(f"new throttling history bits appeared: 0x{new_latched:x}")

    resource_result = verify_resource_stability(resource_log, duration)

    result = {
        "before_throttled": before_text,
        "after_throttled": after_text,
    }
    result.update(metrics_result)
    result.update(resource_result)
    return result


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
