import argparse
import csv
import hashlib
import json
import re
import tempfile
from collections import Counter
from fractions import Fraction
from html.parser import HTMLParser
from pathlib import Path


class StackTables(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.section = None
        self.tables = {}
        self.row = None
        self.cell = None

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        anchor = attrs.get("id", "")
        if tag == "a" and anchor in ("TblP", "TblME", "TblMI", "TblSI", "TblSN", "TblSE"):
            self.section = anchor
            self.tables.setdefault(anchor, [])
        if self.section is None:
            return
        if tag == "tr":
            self.row = {"kind": attrs.get("class", ""), "cells": []}
        elif tag in ("td", "th") and self.row is not None:
            self.cell = []
        elif tag == "br" and self.cell is not None:
            self.cell.append(" ")
        elif self.cell is not None and tag not in ("a", "td", "th", "tr"):
            # xperf emits unescaped C++ template names inside its HTML cells.
            self.cell.append(self.get_starttag_text())

    def handle_data(self, data):
        if self.cell is not None:
            self.cell.append(data)

    def handle_endtag(self, tag):
        if tag in ("td", "th") and self.cell is not None:
            self.row["cells"].append("".join(self.cell).strip())
            self.cell = None
        elif tag == "tr" and self.row is not None:
            if self.row["cells"]:
                self.tables[self.section].append(self.row)
            self.row = None
            self.cell = None


def clock_calibration(path):
    points = []
    for line in path.read_text(encoding="latin-1").splitlines():
        if not line.startswith("# "):
            continue
        fields = [field.strip() for field in line.split("#")]
        if len(fields) >= 4 and fields[2].startswith("0x"):
            point = (int(fields[2], 16), int(fields[3]))
            if not points or point[1] != points[-1][1]:
                points.append(point)
    if len(points) < 2:
        raise ValueError("Clock calibration needs two distinct raw timestamps")
    first, last = points[0], points[-1]
    frequency = Fraction((last[0] - first[0]) * 1_000_000_000, last[1] - first[1])
    if frequency.denominator != 1:
        raise ValueError("ETW clock conversion is not an exact frequency")
    frequency = int(frequency)
    anchor = first[0] * 1_000_000_000 // frequency - first[1]
    error = max(abs(raw * 1_000_000_000 // frequency - ns - anchor) for raw, ns in points)
    if error > 100:
        raise ValueError("ETW clock anchors disagree")
    result = {"frequency": frequency, "steadyOriginNs": anchor, "maxErrorNs": error}
    for row in dump_rows(path):
        if len(row) >= 2 and row[0].strip() == "FirstReliableCSwitchEventTimeStamp" and row[1].strip().isdigit():
            result["firstReliableCSwitchUs"] = int(row[1])
    return result


def timer_paths(folder, clock):
    rows = []
    for line in (folder / "CPU-runtime.log").read_text(encoding="utf-8-sig").splitlines():
        if not line.startswith("TIMER_TRACE "):
            continue
        event = {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)", line)}
        sleep_valid = event["sleep_end_ms"] > 0 and event["sleep_end_ms"] <= event["poll_ms"]
        event["relative_execute_ms"] = event["execute_ms"] - clock["steadyOriginNs"] / 1_000_000
        event["wheel_round_ms"] = event["eligible_ms"] - event["deadline_ms"]
        event["eligible_to_poll_ms"] = event["poll_ms"] - event["eligible_ms"]
        event["selection_ms"] = event["selected_ms"] - event["poll_ms"]
        event["batch_before_ms"] = event["execute_ms"] - event["selected_ms"]
        event["sleep_to_poll_ms"] = event["poll_ms"] - event["sleep_end_ms"] if sleep_valid else None
        if event["wheel_round_ms"] + event["eligible_to_poll_ms"] + event["selection_ms"] + event["batch_before_ms"] != event["delay_ms"]:
            raise ValueError("Timer phase sum does not match lateness")
        rows.append(event)
    return rows


def dump_rows(path):
    # Native xperf CSV may contain ANSI process names; relevant event fields are ASCII.
    with path.open(encoding="latin-1", newline="") as stream:
        yield from csv.reader(stream, skipinitialspace=True)


def ip_samples(folder, pid, tables):
    base = None
    for row in dump_rows(folder / "export/process-images.txt"):
        if len(row) > 10 and row[2].strip() == "Image" and f"({pid})" in row[4].replace(" ", ""):
            if row[10].strip().lower() == "areacpuprobe.exe":
                base = int(row[5], 16)
                break
    if base is None:
        raise ValueError("Target image load address not found")
    samples = Counter()
    total = 0
    for row in dump_rows(folder / "export/cpu-ip.csv"):
        if len(row) > 4 and row[0].strip() == "SampledProfile" and f"({pid})" in row[2].replace(" ", ""):
            total += 1
            samples[int(row[4], 16) - base] += 1
    targets = {}
    for table_row in tables["TblSE"]:
        cells = table_row["cells"]
        if len(cells) < 7 or not cells[4].startswith("0x"):
            continue
        if cells[0] in ("AreaCpuProbe.exe!Area::update_at", "AreaCpuProbe.exe!RadiusArea::try_acquire"):
            low, high = int(cells[4], 16), int(cells[5], 16)
            selected = Counter({ip: count for ip, count in samples.items() if low <= ip < high})
            targets[cells[0]] = {"total": sum(selected.values()), "topRvas": [(hex(ip), count) for ip, count in selected.most_common(12)]}
    return {"imageBase": hex(base), "totalSamples": total, "targets": targets}


def scheduling(folder, pid, clock):
    ranked = []
    for row in dump_rows(folder / "export/thread-cpu.txt"):
        if len(row) == 3 and row[0].strip().isdigit() and f"({pid})" in row[1].replace(" ", ""):
            ranked.append((int(row[0]), int(row[2])))
    tids = {tid for _, tid in sorted(ranked, reverse=True)[:8]}
    segments = {tid: [] for tid in tids}
    running, off = {}, {}
    end = 0
    reliable_start = clock.get("firstReliableCSwitchUs", 0)
    intervals = []
    for row in dump_rows(folder / "export/scheduling.csv"):
        if len(row) < 2 or not row[1].strip().isdigit():
            continue
        time = int(row[1])
        if row[0].strip() == "FirstReliableCSwitchEventTimeStamp":
            reliable_start = time
            continue
        if row[0].strip() == "ReadyThread" and len(row) > 5:
            tid = int(row[5])
            if tid in off and off[tid][3] is None and time >= off[tid][0]:
                off[tid][3] = time
            end = max(end, time)
        elif row[0].strip() == "CSwitch" and len(row) > 13:
            new, old = int(row[3]), int(row[9])
            if old in tids:
                if old in running:
                    segments[old].append((running.pop(old), time, "Running"))
                state, reason = row[12].strip(), row[13].strip()
                off[old] = [time, state, reason, time if state == "Ready" else None]
            if new in tids:
                if new in off:
                    start, state, reason, ready = off.pop(new)
                    intervals.append({"tid": new, "outUs": start, "inUs": time, "oldState": state,
                                      "reason": reason, "readyUs": ready, "offCpuUs": time - start,
                                      "waitingUs": ready - start if ready is not None and state == "Waiting" else None,
                                      "readyDelayUs": time - ready if ready is not None else None})
                    if ready is not None:
                        segments[new].append((start, ready, "Waiting" if state == "Waiting" else state))
                        segments[new].append((ready, time, "Ready"))
                    else:
                        segments[new].append((start, time, "Unknown"))
                running[new] = time
            end = max(end, time)
    for tid, start in running.items():
        segments[tid].append((start, end, "Running"))
    intervals = [row for row in intervals if row["outUs"] >= reliable_start]
    with (folder / "export/offcpu.csv").open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(intervals[0]))
        writer.writeheader()
        writer.writerows(intervals)
    totals = Counter()
    for parts in segments.values():
        for start, finish, state in parts:
            totals[state] += max(0, finish - max(start, reliable_start))
    samples = []
    pending = None
    for line in (folder / "CPU-runtime.log").read_text(encoding="utf-8-sig").splitlines():
        if line.startswith("RADIUS_LATE "):
            pending = {key: int(value) for key, value in re.findall(r"(\w+)=(-?\d+)(?:\s|$)", line)}
        elif line.startswith("RADIUS_PHASE_LATE ") and pending is not None:
            fields = {key: float(value) for key, value in re.findall(r"(\w+)=(-?[\d.]+)", line)}
            start = (pending["queued_ns"] - clock["steadyOriginNs"]) / 1000
            finish = start + (fields["publish_ms"] + fields["dispatch_ms"]) * 1000
            if max(200_000, reliable_start) <= start < finish <= 10_000_000:
                overlap = Counter()
                for parts in segments.values():
                    for left, right, state in parts:
                        overlap[state] += max(0, min(finish, right) - max(start, left))
                budget = (finish - start) * len(tids)
                samples.append({"x": pending["x"], "y": pending["y"], "startMs": start / 1000,
                                "firstProbeMs": finish / 1000, "dispatchMs": fields["dispatch_ms"],
                                "workerStatePercent": {key: 100 * value / budget for key, value in overlap.items()}})
            pending = None
    summary = {"topCpuTids": sorted(tids), "durationUs": end, "reliableStartUs": reliable_start, "stateUs": dict(totals),
               "longestIntervals": sorted(intervals, key=lambda row: row["offCpuUs"], reverse=True)[:12],
               "queueExamples": samples[:3]}
    return summary


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", default="artifacts/radius-profiles-admin")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    root = Path(__file__).resolve().parents[2] / args.input
    for case in ("capacity", "hol", "latency"):
        folder = root / case
        tables = StackTables()
        stack_path = folder / "export/cpu-stacks-resolved.html"
        if not stack_path.exists():
            stack_path = folder / "export/cpu-stacks.html"
        html = stack_path.read_text(encoding="utf-8-sig")
        if not html.rstrip().endswith("</html>"):
            raise ValueError("Incomplete stack export")
        tables.feed(html)
        metadata = json.loads((folder / "CPU.json").read_text(encoding="utf-8-sig"))
        digest = hashlib.sha256((folder / "bin/AreaCpuProbe.exe").read_bytes()).hexdigest()
        for item in csv.DictReader((folder / "binaries.csv").open(encoding="utf-8-sig", newline="")):
            binary = folder / "bin" / Path(item["Path"]).name
            if hashlib.sha256(binary.read_bytes()).hexdigest().upper() != item["Hash"]:
                raise ValueError(f"Recorded binary hash mismatch: {binary}")
        losses = re.findall(r"(?:Lost Buffers|Lost Events)\s*:\s*(\d+)",
                            (folder / "export/trace-statistics.txt").read_text(encoding="utf-8-sig"))
        if not losses or any(int(value) != 0 for value in losses):
            raise ValueError("Trace losses unavailable or nonzero")
        process_rows = [row["cells"] for row in tables.tables["TblP"] if row["kind"] == "pp"]
        if len(process_rows) != 1 or int(process_rows[0][1]) != metadata["pid"]:
            raise ValueError("Stack report PID does not match the recorded target")
        output = {"target": metadata, "exeSha256": digest, "stackSource": stack_path.name, "tables": tables.tables}
        own_rows = [row["cells"] for row in tables.tables["TblSE"] if len(row["cells"]) >= 7 and row["cells"][1].isdigit()]
        if sum(int(row[1]) for row in own_rows) != int(process_rows[0][2]):
            raise ValueError("Exclusive CPU sample sum does not match the process total")
        with (folder / "export/functions.csv").open("w", encoding="utf-8", newline="") as stream:
            writer = csv.writer(stream)
            writer.writerow(("function", "exclusiveSamples", "exclusivePercent", "inclusiveSamples", "rvaStart", "rvaEnd"))
            for row in own_rows:
                writer.writerow((row[0], row[1], row[2], row[3], row[4], row[5]))
        if case == "capacity" and (folder / "export/cpu-ip.csv").exists():
            output["ipSamples"] = ip_samples(folder, metadata["pid"], tables.tables)
            print("IP", json.dumps(output["ipSamples"]))
        clock_path = folder / "export/clock-header.csv"
        if clock_path.exists():
            clock = clock_calibration(clock_path)
            paths = timer_paths(folder, clock)
            output["clock"] = clock
            output["timerPaths"] = paths
            if paths:
                with (folder / "export/timer-paths.csv").open("w", encoding="utf-8", newline="") as stream:
                    writer = csv.DictWriter(stream, fieldnames=list(paths[0]))
                    writer.writeheader()
                    writer.writerows(paths)
                print("TIMERS", json.dumps({"count": len(paths), "clock": clock, "worst": sorted(paths, key=lambda row: row["delay_ms"], reverse=True)[:1]}))
            if (folder / "export/scheduling.csv").exists():
                output["scheduling"] = scheduling(folder, metadata["pid"], clock)
                print("SCHEDULING", case, json.dumps(output["scheduling"]))
        (folder / "export/stack-tables.json").write_text(json.dumps(output, ensure_ascii=False, indent=2), encoding="utf-8")
        print(case, "sha256", digest)
        for section in ("TblP", "TblME", "TblSI", "TblSE"):
            print(section, json.dumps(tables.tables.get(section, [])[:5], ensure_ascii=True))


def self_test():
    tables = StackTables()
    tables.feed("<a id='TblSE'></a><table><tr><tr><td>App!RuntimeBackend<MyArea>::update</td><td>2</td></tr></table>")
    assert tables.tables["TblSE"][0]["cells"] == ["App!RuntimeBackend<MyArea>::update", "2"]
    tests_root = Path(__file__).resolve().parents[2] / "artifacts/radius-profiles-admin/analysis-tests"
    tests_root.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=tests_root, prefix="etw-") as temp:
        folder = Path(temp).resolve()
        assert folder.is_relative_to(tests_root)
        (folder / "export").mkdir()
        clock_file = folder / "clock.txt"
        clock_file.write_text("# 0 # 0xF4240 # 0 # 0\n# 0 # 0xF6950 # 1000000 # 0\n", encoding="ascii")
        assert clock_calibration(clock_file) == {"frequency": 10000000, "steadyOriginNs": 100000000, "maxErrorNs": 0}
        (folder / "export/thread-cpu.txt").write_text("100, AreaCpuProbe.exe (7), 10\n", encoding="ascii")
        (folder / "CPU-runtime.log").write_text("", encoding="ascii")

        def switch(time, new, old, state):
            return ["CSwitch", time, "AreaCpuProbe.exe (7)", new, 9, -1, 0, 0,
                    "AreaCpuProbe.exe (7)", old, 9, -1, state, "Executive"]

        with (folder / "export/scheduling.csv").open("w", encoding="ascii", newline="") as stream:
            csv.writer(stream).writerows((switch(100, 10, 0, "Running"), switch(200, 0, 10, "Waiting"),
                                         ["ReadyThread", 500, "Unknown", 0, "AreaCpuProbe.exe (7)", 10],
                                         switch(700, 10, 0, "Running")))
        result = scheduling(folder, 7, {"steadyOriginNs": 0})
        assert result["stateUs"]["Waiting"] == 300
        assert result["stateUs"]["Ready"] == 200
        assert result["stateUs"]["Running"] == 100
    print("ETW parser, clock calibration and scheduling classification tests passed")


if __name__ == "__main__":
    main()
