"""Audit one F11 client log: nine arena windows, or a pair with markers off/on."""

import argparse
import hashlib
import json
import math
import re
from pathlib import Path


MODES = ("flat", "light", "detailed")
LANES = ("1", "3", "4")
EVENT = re.compile(r"\bCOMMANDER_(BEGIN|RESULT|INVALID)\s+(.+)")
MARKER_EVENT = re.compile(r"\bCOMMANDER_MARKERS_(BEGIN|RESULT|INVALID)\s+(.+)")
VIDEO = re.compile(r"\bCOMMANDER_VIDEO seat=(\d+) decoded=(\d+x\d+) texture=(\d+x\d+)")


def positive(fields, name):
    value = int(fields[name])
    if value <= 0:
        raise ValueError(f"{name} must be positive")
    return value


def read_result(fields):
    if fields["valid"] != "true" or int(fields["seconds"]) != 60:
        raise ValueError("window was invalid or did not cover 60 seconds")
    presents = positive(fields, "presents")
    counts = [int(fields[f"f{lane}"]) for lane in LANES]
    over = int(fields["over33"])
    if any(n < 0 or n > presents for n in counts) or not 0 <= over <= presents + 1:
        raise ValueError("inconsistent frame or interval count")
    p95, longest, *gaps = [float(fields[k]) for k in ("p95_ms", "longest_ms", *(f"gap{i}_ms" for i in LANES))]
    if not all(math.isfinite(n) and 0 <= n <= 60000 for n in [p95, longest, *gaps]) or not 0 < p95 <= longest:
        raise ValueError("inconsistent frame timing")
    return dict(presents=presents, frames=counts, p95_ms=p95, longest_ms=longest,
                over33=over, longest_lane_gaps_ms=gaps)


def analyze(lines, *, markers=False):
    modes = ("off", "on") if markers else MODES
    rounds = (1,) if markers else (1, 2, 3)
    baseline = modes[0]
    events = MARKER_EVENT if markers else EVENT
    expected = [(r, m) for r in rounds for m in modes]
    starts, results, errors = {}, {}, []
    video_samples, raw_events = {}, []
    current = None
    for line_no, line in enumerate(lines, 1):
        match = events.search(line)
        if not match:
            if markers and current is not None:
                video = VIDEO.search(line)
                if video:
                    seat, decoded, _ = video.groups()
                    video_samples[current].setdefault(seat, set()).add(decoded)
            continue
        if markers:
            raw_events.append(line.strip())
        event, text = match.groups()
        try:
            pairs = [part.split("=", 1) for part in text.split()]
            fields = dict(pairs)
            if len(fields) != len(pairs):
                raise ValueError("repeated field")
            if markers and fields["mode"] != "light":
                raise ValueError("the marker comparison requires the same light arena in both windows")
            key = (int(fields["round"]), fields["markers" if markers else "mode"])
            if key not in expected:
                raise ValueError(f"unexpected round or mode {key}")
            if event == "INVALID":
                raise ValueError(f"{key} {fields.get('reason', 'invalid')}")
            if event == "BEGIN":
                if key in starts or current is not None:
                    raise ValueError(f"duplicate or overlapping start for {key}")
                if markers and key != expected[len(starts)]:
                    raise ValueError("markers must be off in the first window and on in the second")
                starts[key] = [positive(fields, "width"), positive(fields, "height")]
                current = key
                if markers:
                    video_samples[key] = {}
            else:
                if current != key or key not in starts or key in results:
                    raise ValueError(f"result without matching unique BEGIN {key}")
                current = None
                results[key] = read_result(fields)
                if markers and any(video_samples[key].get(seat) != {"1920x1080"} for seat in LANES):
                    errors.append(f"line {line_no}: all three decoders must report 1920x1080 during {key[1]}")
        except (ValueError, KeyError) as exc:
            errors.append(f"line {line_no}: {exc}")

    missing = [f"{r}/{m}" for r, m in expected if (r, m) not in results]
    if missing:
        errors.append("missing valid results: " + ", ".join(missing))
    dimensions = sorted({tuple(size) for size in starts.values()})
    if len(dimensions) != 1:
        errors.append("expected one common framebuffer size")
    elif markers and dimensions != [(1920, 1080)]:
        errors.append("marker benchmark requires a 1920x1080 F11 framebuffer")
    for (r, mode), row in results.items():
        if mode == baseline and min(row["frames"]) == 0:
            errors.append(f"round {r}: zero-frame lane in {baseline} baseline")
    candidates = {}
    for mode in modes[1:]:
        failed = []
        for r in rounds:
            if (r, mode) not in results or (r, baseline) not in results:
                continue
            row, base = results[r, mode], results[r, baseline]["frames"]
            if min(base) <= 0:
                continue
            row[f"percent_of_{baseline}"] = [round(f * 100 / b, 2) for f, b in zip(row["frames"], base)]
            for lane, f, b in zip(LANES, row["frames"], base):
                if 100 * f < 95 * b:
                    failed.append(f"round {r}: lane {lane} {f}/{b} below 95%")
            # Client prints the percentile rounded to 3 decimals. 'over33' is
            # counted before rounding and includes the beginning/end gaps.
            if row["p95_ms"] > 33 or row["over33"] * 20 > row["presents"] + 1:
                failed.append(f"round {r}: more than 5% of frame intervals exceed 33 ms")
        candidates[mode] = dict(status="unverified" if errors else ("fail" if failed else "pass"),
                                failed_checks=failed)
        worst = [(value, r, lane) for (r, m), row in results.items() if m == mode
                 for lane, value in zip(LANES, row["longest_lane_gaps_ms"])]
        if worst:
            value, r, lane = max(worst)
            candidates[mode]["longest_video_freeze"] = dict(ms=value, round=r, lane=lane)
    windows = [dict(round=r, mode=m, **results[r, m]) for r, m in expected if (r, m) in results]
    report = dict(complete=not errors, errors=errors, framebuffer_sizes=dimensions,
                  lane_order=list(LANES), windows=windows, candidates=candidates)
    if markers:
        for window in windows:
            window["markers"] = window["mode"]
            window["mode"] = "light"
        report["seconds_per_window"] = 60
        report["log_events"] = raw_events
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("log", type=Path, help="one robogo.log")
    parser.add_argument("--markers", action="store_true",
                        help="audit the 1920x1080 light-arena marker comparison (off and on)")
    parser.add_argument("--output", type=Path, help="write the same audit to a JSON file")
    args = parser.parse_args()
    if args.output and args.output.resolve() == args.log.resolve():
        parser.error("The output must be different from the source log.")
    raw = args.log.read_bytes()
    report = analyze(raw.decode("utf-8-sig").splitlines(), markers=args.markers)
    report["log_sha256"] = hashlib.sha256(raw).hexdigest()
    serialized = json.dumps(report, ensure_ascii=False, indent=2, allow_nan=False)
    if args.output:
        args.output.write_text(serialized + "\n", encoding="utf-8")
        print("round setting   frames(1/3/4)       P95_ms  longest_1/3/4_ms")
        for row in report["windows"]:
            counts = "/".join(map(str, row["frames"]))
            gaps = "/".join(f"{n:.1f}" for n in row["longest_lane_gaps_ms"])
            setting = row.get("markers", row["mode"])
            print(f"{row['round']:5d} {setting:9s} {counts:18s} {row['p95_ms']:7.3f}  {gaps}")
        for mode, result in report["candidates"].items():
            print(f"{mode}: {result['status']}")
        for error in report["errors"]:
            print(error)
    else:
        print(serialized)
    return 2 if not report["complete"] else int(any(v["status"] == "fail" for v in report["candidates"].values()))


if __name__ == "__main__":
    raise SystemExit(main())
