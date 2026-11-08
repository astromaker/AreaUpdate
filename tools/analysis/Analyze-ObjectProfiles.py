import argparse
import csv
import hashlib
import importlib.util
import json
import re
from collections import Counter
from pathlib import Path


SPEC = importlib.util.spec_from_file_location("radius_etw", Path(__file__).with_name("Analyze-RadiusEtw.py"))
HELPERS = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(HELPERS)


def read_tables(path, pid):
    text = path.read_text(encoding="utf-8-sig")
    if not text.rstrip().endswith("</html>"):
        raise ValueError(f"Incomplete stack report: {path}")
    parser = HELPERS.StackTables()
    parser.feed(text)
    process = [row["cells"] for row in parser.tables["TblP"] if row["kind"] == "pp"]
    if len(process) != 1 or int(process[0][1]) != pid:
        raise ValueError(f"Unexpected process: {path}")
    total = int(process[0][2])
    functions = []
    for row in parser.tables["TblSE"]:
        cells = row["cells"]
        if len(cells) >= 7 and cells[1].isdigit():
            functions.append({"function": cells[0], "own": int(cells[1]), "inclusive": int(cells[3]),
                              "rva": cells[4], "limit": cells[5]})
    if sum(row["own"] for row in functions) != total or total == 0:
        raise ValueError(f"Exclusive sample sum mismatch: {path}")
    return total, functions


def category(name):
    if re.search(r"Etwp|EtwTrace|Unwind|WalkFrameChain|LookupFunctionEntryForStackWalks", name):
        return "stackwalking_tracing"
    if re.search(r"Heap|malloc|free_base|!operator new|!operator delete", name):
        return "heap"
    if "RtlSleepConditionVariable" in name:
        return "condition_variable"
    if re.search(r"QueryPerformanceCounter|steady_clock", name):
        return "clock"
    if "!TestObject::update" in name:
        return "actor_body"
    if "Unknown" in name or "!0x" in name:
        return "unresolved"
    return "other"


def trace_duration(text):
    match = re.search(r"\(\+\s*(\d+):(\d+):(\d+):(\d+\.\d+)\)", text)
    if not match:
        raise ValueError("Trace duration is missing")
    days, hours, minutes, seconds = map(float, match.groups())
    return days * 86400 + hours * 3600 + minutes * 60 + seconds


def cpu_threads(path, pid):
    process_us = None
    threads = []
    for row in csv.reader(path.open(encoding="utf-8-sig", newline="")):
        if len(row) < 2 or not row[0].strip().isdigit() or not re.search(rf"\(\s*{pid}\s*\)", row[1]):
            continue
        if len(row) == 2:
            process_us = int(row[0])
        elif len(row) == 3:
            threads.append({"tid": int(row[2]), "cpuUs": int(row[0])})
    if process_us is None or abs(sum(row["cpuUs"] for row in threads) - process_us) > 2 * len(threads):
        raise ValueError("Context-switch CPU sum mismatch")
    return process_us, sorted(threads, key=lambda row: row["cpuUs"], reverse=True)


def write_csv(path, rows):
    if not rows:
        return
    with path.open("w", encoding="utf-8-sig", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=list(rows[0]))
        writer.writeheader()
        writer.writerows(rows)


def scheduling_events(events, tids, start, end, clip_end=False):
    state = {tid: "Unknown" for tid in tids}
    since = {tid: start for tid in tids}
    totals = {tid: Counter() for tid in tids}
    switches, migrations, wait_reasons = Counter(), Counter(), Counter()
    last_cpu, ready_delays = {}, Counter()
    last_time = -1

    def transition(tid, time, new_state):
        if time < start:
            state[tid], since[tid] = new_state, time
            return
        old_state = state[tid]
        totals[tid][old_state] += time - max(start, since[tid])
        if old_state == "Ready" and new_state == "Running" and since[tid] >= start:
            ready_delays[time - since[tid]] += 1
        state[tid], since[tid] = new_state, time

    for kind, time, first, second, old_state, reason, cpu in events:
        if time < last_time:
            raise ValueError("Scheduling events are not chronological")
        last_time = time
        if time > end:
            break
        if kind == "CSwitch":
            new, old = first, second
            if old in tids:
                transition(old, time, old_state if old_state in ("Ready", "Waiting") else "Unknown")
                if time >= start:
                    switches[old] += 1
                    if old_state == "Waiting":
                        wait_reasons[reason] += 1
            if new in tids:
                transition(new, time, "Running")
                if time >= start and new in last_cpu and last_cpu[new] != cpu:
                    migrations[new] += 1
                last_cpu[new] = cpu
        elif first in tids and state[first] not in ("Running", "Ready"):
            transition(first, time, "Ready")
    if clip_end:
        end = min(end, last_time)
    if end <= start:
        raise ValueError("Empty scheduling window")
    rows, combined = [], Counter()
    for tid in sorted(tids):
        totals[tid][state[tid]] += end - max(start, since[tid])
        if sum(totals[tid].values()) != end - start:
            raise ValueError("Thread scheduling budget mismatch")
        combined.update(totals[tid])
        rows.append({"tid": tid, **{name + "Us": totals[tid][name] for name in ("Running", "Ready", "Waiting", "Unknown")},
                     "switchOuts": switches[tid], "migrations": migrations[tid]})
    count, cumulative, p99 = sum(ready_delays.values()), 0, None
    for delay, hits in sorted(ready_delays.items()):
        cumulative += hits
        if cumulative >= count * 0.99:
            p99 = delay
            break
    budget = (end - start) * len(tids)
    summary = {name + "Percent": combined[name] * 100 / budget for name in ("Running", "Ready", "Waiting", "Unknown")}
    summary.update({"startUs": start, "endUs": end, "workerCount": len(tids),
                    "switchOuts": sum(switches.values()), "migrations": sum(migrations.values()),
                    "switchOutsPerSecond": sum(switches.values()) / ((end - start) / 1e6),
                    "readyToRunCount": count, "readyToRunP99Us": p99,
                    "readyToRunMeanUs": sum(delay * hits for delay, hits in ready_delays.items()) / count if count else None,
                    "waitReasons": dict(wait_reasons)})
    return summary, rows


def analyze_scheduling(path, tids, duration):
    reliable, last_event = 200000, 0

    def events():
        nonlocal reliable, last_event
        for row in csv.reader(path.open(encoding="utf-8-sig", newline="")):
            if len(row) < 2 or not row[1].strip().isdigit():
                continue
            kind, time = row[0].strip(), int(row[1])
            if kind == "FirstReliableCSwitchEventTimeStamp":
                reliable = max(reliable, time)
            elif kind == "CSwitch" and len(row) > 16:
                last_event = max(last_event, time)
                yield kind, time, int(row[3]), int(row[9]), row[12].strip(), row[13].strip(), int(row[16])
            elif kind == "ReadyThread" and len(row) > 5:
                last_event = max(last_event, time)
                yield kind, time, int(row[5]), 0, "", "", 0

    import itertools
    iterator = events()
    first = next(iterator)
    end = round(duration * 1e6) - 100000
    summary, rows = scheduling_events(itertools.chain((first,), iterator), tids, reliable, end, clip_end=True)
    if reliable > summary["startUs"]:
        raise ValueError("Reliable timestamp was reported after scheduling data")
    write_csv(path.with_name("object-thread-states.csv"), rows)
    return summary


def analyze_case(folder):
    metadata = json.loads((folder / "CPU.json").read_text(encoding="utf-8-sig"))
    if metadata["validateOnly"]:
        raise ValueError("Preflight is not a CPU recording")
    digest = hashlib.sha256((folder / "bin/AreaCpuProbe.exe").read_bytes()).hexdigest().upper()
    if digest != metadata["executableSha256"]:
        raise ValueError("Measured executable was modified")
    export = folder / "export"
    stats = (export / "trace-statistics.txt").read_text(encoding="utf-8-sig")
    losses = re.findall(r"(?:Lost Buffers|Lost Events)\s*:\s*(\d+)", stats)
    if not losses or any(int(value) for value in losses):
        raise ValueError("Trace losses are unavailable or nonzero")
    total, functions = read_tables(export / "cpu-stacks.html", metadata["pid"])
    if metadata.get("schedulingStacks", True):
        wait_count, wait_functions = read_tables(export / "wait-stacks.html", metadata["pid"])
    else:
        wait_count, wait_functions = None, []
    process_us, threads = cpu_threads(export / "thread-cpu.txt", metadata["pid"])
    workers = {row["tid"] for row in threads[:16]}
    if len(workers) != 16:
        raise ValueError("Expected 16 timer/Dispatcher worker threads")
    ready = Counter()
    for row in csv.reader((export / "ready-thread.txt").open(encoding="utf-8-sig", newline="")):
        if len(row) >= 3 and row[2].strip().isdigit() and int(row[2]) in workers:
            ready[int(row[2])] += 1
    duration = trace_duration(stats)
    counts = Counter()
    for row in functions:
        counts[category(row["function"])] += row["own"]
        row["ownPercent"] = row["own"] * 100 / total
        row["inclusivePercent"] = row["inclusive"] * 100 / total
    log = (folder / "CPU-runtime.log").read_text(encoding="utf-8-sig")
    runtime = {}
    for line in log.splitlines():
        if line.startswith(("STEADY ", "SUMMARY ", "TARGET ", "RUNTIME ")):
            runtime.update(dict(re.findall(r"(\w+)=([^ ]+)", line)))
    for field in ("overlaps", "never_updated", "tasks_live", "tasks_pending"):
        if runtime.get(field) != "0":
            raise ValueError(f"Runtime invariant failed: {field}")
    result = {"case": folder.name, "actors": metadata["actors"], "version": metadata["version"],
              "seed": metadata["seed"], "pid": metadata["pid"], "traceSeconds": duration,
              "cpuStackSamples": total, "cswitchStackCount": wait_count,
              "cswitchStackRate": wait_count / duration if wait_count is not None else None, "workerReadyEvents": sum(ready.values()),
              "workerReadyRate": sum(ready.values()) / duration,
              "cswitchCpuSeconds": process_us / 1e6,
              "etlSpanNormalizedCpuPercent": process_us / (duration * 1e6 * 8) * 100,
              "profileRunFps": float(runtime["steady_actor_fps"])}
    for name in ("stackwalking_tracing", "heap", "condition_variable", "clock", "actor_body", "unresolved", "other"):
        result[name + "Percent"] = counts[name] * 100 / total
    scheduling = None
    if (export / "scheduling.csv").exists():
        scheduling = analyze_scheduling(export / "scheduling.csv", workers, duration)
        for field in ("RunningPercent", "ReadyPercent", "WaitingPercent", "UnknownPercent", "readyToRunMeanUs", "readyToRunP99Us"):
            result[field] = scheduling[field]
    write_csv(export / "object-functions.csv", functions)
    write_csv(export / "object-wait-functions.csv", wait_functions)
    write_csv(export / "object-thread-cpu.csv", threads)
    (export / "object-analysis.json").write_text(json.dumps({"summary": result, "metadata": metadata,
        "functions": functions, "waitFunctions": wait_functions, "threads": threads, "scheduling": scheduling}, indent=2), encoding="utf-8")
    return result


def self_test():
    assert trace_duration("(+ 0:00:01:14.7517131)") == 74.7517131
    assert category("ntkrnlmp.exe!RtlpUnwindPrologue") == "stackwalking_tracing"
    assert category("ntdll.dll!RtlFreeHeap") == "heap"
    assert category("ntdll.dll!RtlSleepConditionVariableSRW") == "condition_variable"
    assert category("AreaCpuProbe.exe!TestObject::update") == "actor_body"
    assert category("AreaCpuProbe.exe!TaskExecuter::execute") == "other"
    events = [("CSwitch", 0, 1, 0, "Waiting", "", 0), ("CSwitch", 100, 0, 1, "Waiting", "CV", 0),
              ("ReadyThread", 300, 1, 0, "", "", 0), ("CSwitch", 400, 1, 0, "Waiting", "", 1),
              ("CSwitch", 700, 0, 1, "Ready", "", 0), ("CSwitch", 800, 1, 0, "Waiting", "", 1)]
    summary, rows = scheduling_events(events, {1}, 0, 1000)
    assert (rows[0]["RunningUs"], rows[0]["ReadyUs"], rows[0]["WaitingUs"]) == (600, 200, 200)
    assert summary["migrations"] == 1 and summary["readyToRunMeanUs"] == 100
    assert summary["waitReasons"] == {"CV": 1}
    unknown, unknown_rows = scheduling_events(events, {1, 2}, 0, 1000)
    assert unknown["UnknownPercent"] == 50 and unknown_rows[1]["UnknownUs"] == 1000
    clipped, clipped_rows = scheduling_events(events, {1}, 0, 2000, clip_end=True)
    assert clipped["endUs"] == 800 and sum(clipped_rows[0][key] for key in ("RunningUs", "ReadyUs", "WaitingUs", "UnknownUs")) == 800
    boundary = [("CSwitch", 0, 0, 1, "Ready", "", 0), ("CSwitch", 75, 1, 0, "Waiting", "", 0)]
    edge, edge_rows = scheduling_events(boundary, {1}, 50, 100)
    assert edge["readyToRunCount"] == 0 and edge_rows[0]["ReadyUs"] == 25
    print("Object analysis self-test passed")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", default="artifacts/object-profiles-admin")
    parser.add_argument("--allow-incomplete", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return
    root = Path(__file__).resolve().parents[2] / args.input
    rows = []
    for case in csv.DictReader((root / "plan.csv").open(encoding="utf-8-sig", newline="")):
        folder = root / f"{case['actors']}-{case['seed']}-{case['version']}"
        if args.allow_incomplete:
            metadata = json.loads((folder / "CPU.json").read_text(encoding="utf-8-sig"))
            required = [folder / "export/cpu-stacks.html"]
            if metadata.get("schedulingStacks", True):
                required.append(folder / "export/wait-stacks.html")
            if any(not path.exists() or not path.read_text(encoding="utf-8-sig").rstrip().endswith("</html>") for path in required):
                continue
        row = analyze_case(folder)
        rows.append(row)
        print(json.dumps(row))
    write_csv(root / "analysis-summary.csv", rows)


if __name__ == "__main__":
    main()
