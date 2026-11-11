# -*- coding: utf-8 -*-
"""验证同场景系统开关测量；保留慢帧，CPU、GPU 和实际帧间隔分别汇总。"""
import argparse
import csv
import hashlib
import json
import math
import statistics
from pathlib import Path


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))


def metric(values):
    values = sorted(values)
    assert values and all(math.isfinite(v) and v >= 0 for v in values)
    def percentile(p):
        index = (len(values) - 1) * p
        first = math.floor(index)
        last = math.ceil(index)
        return values[first] + (values[last] - values[first]) * (index - first)
    return dict(count=len(values), mean=statistics.mean(values), median=statistics.median(values),
                p95=percentile(.95), p99=percentile(.99), maximum=values[-1])


def validate_run(directory, environment, formal, capture=False):
    managed = read_json(directory / "managed.json")
    native = read_json(directory / "window-profile.json")
    assert managed["success"] and native["success"] and managed["capture"] == capture
    assert managed["source_sha256"] == environment["source_sha256"]
    assert managed["visible"] and native["window_station"] == "WinSta0" and native["thread_desktop"] == "Default"
    assert not native["adapter"]["software"] and not native["overflow"] and native["cpu_dropped_frames"] == 0
    assert native["gpu_export_ok"] and native["final"]["debug_warnings"] == native["final"]["debug_errors"] == 0
    assert native["final"]["global_waits_before_shutdown"] == int(capture)
    assert managed["draw_objects"] == 32 and managed["probe_entities"] == 65
    assert managed["script_count"] == 32 * managed["scripting"] and managed["body_count"] == 33 * managed["physics"]
    assert managed["ui_controls"] == 16 * managed["ui"] and managed["clock"]["Steps"] > 0
    assert managed["updates"] == 32 * managed["observations"] * managed["scripting"]
    assert managed["fixed_updates"] == 32 * managed["clock"]["Steps"] * managed["scripting"]
    if managed["physics"]:
        assert managed["awake_bodies"] == 32 and managed["motion"] > 0
        assert .45 < managed["minimum_body_y"] <= managed["maximum_body_y"] < .6
        if managed["scripting"]:
            assert managed["contacts"] > 0
    if formal:
        assert managed["warmup_seconds"] == 5 and managed["sample_seconds"] == 20
    frames = read_csv(directory / "frames.csv")
    gpu = read_csv(directory / "gpu.csv")
    cpu_rows = read_csv(directory / "cpu.csv")
    observations = read_csv(directory / "managed.csv")
    assert len(frames) == native["rendered_frames"] == len(gpu) == native["final"]["submitted_frames"]
    assert len(observations) == managed["observations"] == native["cpu_frames"]
    assert len(observations) == len(frames) + int(not capture)
    assert [int(r["frame"]) for r in frames] == list(range(1, len(frames) + 1))
    assert [int(r["submission"]) for r in frames] == list(range(1, len(frames) + 1))
    assert [int(r["source_frame"]) for r in gpu] == [int(r["frame"]) for r in frames]
    assert [int(r["submission"]) for r in gpu] == [int(r["submission"]) for r in frames]
    cpu = {}
    for row in cpu_rows:
        key = (int(row["frame"]), row["scope"])
        assert key not in cpu
        cpu[key] = float(row["cpu_ms"])
        assert math.isfinite(cpu[key]) and cpu[key] >= 0
    for row in gpu:
        ticks = [int(row[k]) for k in ("frame_begin", "geometry_begin", "geometry_end", "frame_end")]
        assert ticks == sorted(ticks) and int(row["frequency_hz"]) > 0 and int(row["draws"]) == 32
        assert math.isclose(float(row["gpu_frame_ms"]), (ticks[-1] - ticks[0]) * 1000 / int(row["frequency_hz"]), abs_tol=1e-9)
    for row in frames:
        assert (int(row["width"]), int(row["height"])) == (1280, 720)
        assert int(row["visible"]) == 1 and int(row["draws"]) == 32
        assert int(row["ui_vertices"]) == (96 if managed["ui"] else 0)
        assert int(row["ui_draws"]) == (24 if managed["ui"] else 0)
        assert int(row["memory_used_bytes"]) <= int(row["memory_reserved_bytes"])
        for scope in ("CPU.Frame", "CPU.Environment", "CPU.Update", "CPU.Extract", "CPU.CommandPrepare", "CPU.CommandRecord", "CPU.SubmitPresent"):
            assert (int(row["frame"]), scope) in cpu
    elapsed = [float(r["elapsed_s"]) for r in observations]
    assert elapsed == sorted(elapsed)
    sample_indices = [i for i in range(len(frames)) if capture or managed["warmup_seconds"] <= elapsed[i] < managed["warmup_seconds"] + managed["sample_seconds"]]
    assert len(sample_indices) >= (10 if formal else 2)
    if not capture:
        assert elapsed[-1] >= managed["warmup_seconds"] + managed["sample_seconds"]
    sample_frames = [frames[i] for i in sample_indices]
    metrics = {scope: metric([cpu[(int(r["frame"]), scope)] for r in sample_frames])
               for scope in ("CPU.Frame", "CPU.Environment", "CPU.Update", "CPU.Extract", "CPU.CommandPrepare", "CPU.CommandRecord", "CPU.SubmitPresent")}
    for key in ("gpu_frame_ms", "gpu_geometry_ms"):
        metrics[key] = metric([float(gpu[i][key]) for i in sample_indices])
    metrics["wall_delta_ms"] = metric([float(r["raw_delta_ms"]) for r in sample_frames])
    for key in ("physics_control_ms", "observer_including_control_ms"):
        metrics[key] = metric([float(observations[i][key]) for i in sample_indices])
    span = elapsed[sample_indices[-1]] - elapsed[sample_indices[0]]
    memory = {key: dict(first=int(sample_frames[0][key]), last=int(sample_frames[-1][key]), maximum=max(int(r[key]) for r in sample_frames))
              for key in ("memory_reserved_bytes", "memory_used_bytes", "upload_reserved_bytes", "persistent_descriptors", "tracked_resources")}
    return dict(run=directory.name, scripts=managed["scripting"], physics=managed["physics"], ui=managed["ui"],
                frames=len(frames), sample_frames=len(sample_indices), sampled_span_seconds=span,
                wall_submission_hz=(len(sample_indices) - 1) / span,
                occluded_frames=sum(int(r["occluded"]) for r in sample_frames),
                foreground_fraction=statistics.mean(int(r["foreground"]) for r in sample_frames),
                clamped_frames=sum(float(r["raw_delta_ms"]) > float(r["update_delta_ms"]) + 1e-6 for r in sample_frames),
                dropped_simulation_seconds=managed["clock"]["DroppedSeconds"], metrics=metrics, memory=memory,
                managed_allocated_bytes=managed["managed_allocated_bytes"], gc=[managed[k] for k in ("gc0", "gc1", "gc2")])


def validate(root, formal):
    environment = read_json(root / "environment.json")
    assert environment["suite"] == "System"
    if formal:
        assert environment["configuration"] == "Release" and not environment["smoke"] and environment["repetitions"] >= 3
    runs = [validate_run(path.parent, environment, formal) for path in sorted(root.glob("*/managed.json"))]
    assert len(runs) == environment["repetitions"] * 8
    groups = {}
    images = {}
    for s in range(2):
        for p in range(2):
            for u in range(2):
                name = f"s{s}p{p}u{u}"
                group = [r for r in runs if (r["scripts"], r["physics"], r["ui"]) == (bool(s), bool(p), bool(u))]
                assert len(group) == environment["repetitions"]
                groups[name] = dict(mean_run_medians_ms={key: statistics.mean(r["metrics"][key]["median"] for r in group) for key in group[0]["metrics"]},
                                    run_medians_ms={key: [r["metrics"][key]["median"] for r in group] for key in group[0]["metrics"]},
                                    mean_submission_hz=statistics.mean(r["wall_submission_hz"] for r in group),
                                    occluded_frames=sum(r["occluded_frames"] for r in group))
                validate_run(root / "captures" / name, environment, formal, True)
                images[name] = hashlib.sha256((root / "captures" / (name + ".ppm")).read_bytes()).hexdigest()
    assert len({images[k] for k in images if k.endswith("u0")}) == 1
    assert len({images[k] for k in images if k.endswith("u1")}) == 1
    assert images["s0p0u0"] != images["s0p0u1"]
    # PPM 由引擎固定写出；UI 外像素必须完全相同，不能以开关名称代替图像证据。
    def ppm(path):
        magic, size, maximum, pixels = path.read_bytes().split(b"\n", 3)
        assert (magic, size, maximum) == (b"P6", b"1280 720", b"255") and len(pixels) == 1280 * 720 * 3
        return pixels
    first, last = (ppm(root / "captures" / (name + ".ppm")) for name in ("s0p0u0", "s0p0u1"))
    changed = [(i // 3 % 1280, i // 3 // 1280) for i in range(0, len(first), 3) if first[i:i+3] != last[i:i+3]]
    assert changed and all(0 <= x < 300 and 0 <= y < 650 for x, y in changed)
    effects = {}
    for bit, label in enumerate(("scripts", "physics", "ui")):
        differences = []
        for mask in range(8):
            if mask & (1 << bit):
                continue
            name = lambda value: f"s{value & 1}p{(value >> 1) & 1}u{(value >> 2) & 1}"
            off, on = groups[name(mask)], groups[name(mask | (1 << bit))]
            differences.append({key: on["mean_run_medians_ms"][key] - off["mean_run_medians_ms"][key] for key in off["mean_run_medians_ms"]})
        effects[label] = {key: statistics.mean(d[key] for d in differences) for key in differences[0]}
    interactions = {}
    for first_bit, second_bit, label in ((0, 1, "scripts_physics"), (0, 2, "scripts_ui"), (1, 2, "physics_ui")):
        values = []
        for mask in range(8):
            if mask & ((1 << first_bit) | (1 << second_bit)):
                continue
            name = lambda value: f"s{value & 1}p{(value >> 1) & 1}u{(value >> 2) & 1}"
            a, b, c, d = (groups[name(m)]["mean_run_medians_ms"] for m in (mask, mask | (1 << first_bit), mask | (1 << second_bit), mask | (1 << first_bit) | (1 << second_bit)))
            values.append({key: d[key] - c[key] - b[key] + a[key] for key in a})
        interactions[label] = {key: statistics.mean(v[key] for v in values) for key in values[0]}
    return dict(schema=1, formal_protocol_passed=formal, environment=environment, runs=runs, groups=groups,
                average_paired_switch_delta_ms=effects, paired_interaction_delta_ms=interactions, image_sha256=images,
                ui_changed_bounds=[min(x for x, y in changed), min(y for x, y in changed), max(x for x, y in changed), max(y for x, y in changed)],
                notes="Three-dimensional pixels identical across all switches; UI differences confined to declared rectangle. Effects are controlled workload observations, not causal guarantees for other games. CPU/GPU not additive. Last stop update has no render and is retained in managed/CPU traces.")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--formal", action="store_true")
    args = parser.parse_args()
    result = validate(args.input, args.formal)
    (args.input / "summary.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    lines = ["# 整合系统性能回归", "", "相同 32 个静态物体、65 个非渲染测试实体；所有组保留同一个 ScriptSystem 时钟。脚本开启附着 32 个 Behaviour，物理开启创建 32 动态盒及地面，UI 开启显示 16 个控件。", "", "物理控制回调包含在物理组成本中；空脚本系统及观察器开销为共同底座。GPU 使用实际 timestamps，帧间隔包含等待和帧外休眠。内存为分配器保留/使用字节，不是驻留显存。", "", "| 组合 | CPU Frame 中位均值 ms | Environment ms | GPU ms | 帧间隔 ms | 提交 Hz | occluded |", "|---|---:|---:|---:|---:|---:|---:|"]
    for name, group in result["groups"].items():
        m = group["mean_run_medians_ms"]
        lines.append(f"| {name} | {m['CPU.Frame']:.4f} | {m['CPU.Environment']:.4f} | {m['gpu_frame_ms']:.4f} | {m['wall_delta_ms']:.4f} | {group['mean_submission_hz']:.3f} | {group['occluded_frames']} |")
    lines += ["", "s/p/u 分别为脚本/物理/UI。各轮完整分布、慢帧、GC、观察器、内存和配对开关差值保存在 summary.json。预热按约定排除；采样窗口内的慢帧全部保留。自动性能回归不替代完整 V1 的双系统和真人输入验收。"]
    (args.input / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    files = [{"path": path.relative_to(args.input).as_posix(), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
             for path in sorted(args.input.rglob("*")) if path.is_file() and path.name != "artifact-digests.json"]
    (args.input / "artifact-digests.json").write_text(json.dumps(files, indent=2) + "\n", encoding="utf-8")
    print(f"System protocol validated: {len(result['runs'])} runs; formal={args.formal}.")


if __name__ == "__main__":
    main()
