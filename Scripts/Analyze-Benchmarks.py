"""核对原始帧配对，再按 nearest-rank 计算分位数；不把短烟雾当正式采样。"""
from pathlib import Path
import argparse
import csv
import hashlib
import json
import math
import statistics

def read_csv(path):
    with path.open(encoding="utf-8-sig", newline="") as stream:
        return list(csv.DictReader(stream))

def metric(values):
    assert values and all(math.isfinite(value) and value >= 0 for value in values)
    ordered = sorted(values)
    return {"mean": statistics.mean(values), "median": statistics.median(values),
            "p95": ordered[math.ceil(len(values) * 0.95) - 1],
            "p99": ordered[math.ceil(len(values) * 0.99) - 1]}

def validate(directory, environment, formal):
    run = json.loads((directory / "run.json").read_text(encoding="utf-8"))
    assert run["success"] and run["measurement_complete"] and run["mode"] == "measurement"
    assert run["configuration"] == "Release" and run["source_sha256"] == environment["source_sha256"]
    assert run["executable_sha256"] == environment["executable_sha256"]
    if formal:
        assert run["sample_seconds"] >= 60 and run["warmup_seconds"] >= 5 and not environment["smoke"]
    assert run["elapsed_seconds"] >= run["warmup_seconds"] + run["sample_seconds"]
    frames, cpu, gpu = (read_csv(directory / name) for name in ("frames.csv", "cpu.csv", "gpu.csv"))
    frame_map = {int(row["frame"]): row for row in frames}
    gpu_map = {int(row["source_frame"]): row for row in gpu}
    assert len(frame_map) == len(frames) == run["rendered_frames"] == run["cpu_frames_streamed"] == run["gpu_frames_streamed"]
    assert len(gpu_map) == len(gpu) and frame_map.keys() == gpu_map.keys()
    cpu_map = {}
    for row in cpu:
        scope = cpu_map.setdefault(int(row["frame"]), {})
        assert row["scope"] not in scope
        scope[row["scope"]] = float(row["cpu_ms"])
    assert cpu_map.keys() == frame_map.keys() and all("CPU.Frame" in row for row in cpu_map.values())
    for index, frame in frame_map.items():
        count, visible, culled = run["object_count"], int(frame["visible"]), int(frame.get("culled", 0))
        assert int(frame["candidates"]) == visible + culled == count
        draw_count = int(frame["draws"])
        assert int(gpu_map[index]["draws"]) == draw_count
        if run["instancing"]:
            assert int(frame["instances"]) == visible and int(frame["instance_batches"]) == draw_count
            assert int(frame["instance_bytes"]) == visible * 240 and int(frame["instance_capacity"]) >= visible
            assert int(frame["opaque_instance_buffer_binds"]) == draw_count
            assert (0 < draw_count <= min(visible, 48)) if visible else draw_count == 0
            if run["scene"] == "repeated":
                assert draw_count == min(visible, 1)
        else:
            assert draw_count == visible
            if "instance_bytes" in frame:
                assert int(frame["instance_bytes"]) == int(frame["opaque_instance_buffer_binds"]) == 0
        if not run["culling"]:
            assert culled == 0
        elif run.get("visibility_case") == "all_visible":
            assert visible == count
        elif run.get("visibility_case") == "mostly_outside":
            assert visible == (count + 9) // 10
        assert int(frame["debug_errors"]) == 0 and int(frame["shader_compilations"]) == 0
        if "opaque_pipeline_binds" in frame:
            assert int(frame["opaque_pipeline_binds"]) == int(frame["opaque_root_binds"]) == (1 if draw_count else 0)
            assert int(frame["opaque_constant_binds"]) == draw_count
            for key in ("opaque_table_binds", "opaque_vertex_binds", "opaque_index_binds"):
                actual_binds = int(frame[key])
                assert 0 <= actual_binds <= draw_count
                if not run.get("binding_cache", False):
                    assert actual_binds == draw_count
            if "draw_constant_bytes" in frame:
                assert int(frame["draw_constant_bytes"]) == draw_count * 512
        ticks = [int(gpu_map[index][name]) for name in ("frame_begin", "geometry_begin", "geometry_end", "frame_end")]
        assert ticks == sorted(ticks) and int(gpu_map[index]["frequency_hz"]) > 0
        side = 1
        while side ** 3 < run["object_count"]:
            side += 1
        path = run["camera_path"]
        angle = math.remainder(float(frame["sample_time_s"]) / path["period_seconds"] * 2 * math.pi, 2 * math.pi)
        radius = side * path["radius_multiplier"]
        height = side * path["height_multiplier"]
        expected = (radius * math.sin(angle), height, radius * math.cos(angle))
        actual = tuple(float(frame[key]) for key in ("camera_x", "camera_y", "camera_z"))
        assert all(math.isfinite(value) for value in actual)
        assert max(abs(a - b) for a, b in zip(actual, expected)) < 0.0005, "camera route disagrees with recorded time"
        assert abs(float(frame["camera_pitch"]) + math.atan2(height, radius)) < 0.00001
    sample = {index: row for index, row in frame_map.items() if row["phase"] == "sample"}
    assert len(sample) >= (100 if formal else 1)
    times = [float(row["sample_time_s"]) for row in sample.values()]
    assert times == sorted(times) and min(times) >= 0 and max(times) < run["sample_seconds"]
    if formal:
        assert min(times) < 1 and max(times) > run["sample_seconds"] - 1
    for key in ("global_waits", "memory_resources", "memory_used_bytes", "pipeline_creations", "upload_bytes"):
        assert len({row[key] for row in sample.values()}) == 1, (directory.name, "sample changed", key)
    if run["culling"]:
        assert all(int(row["bounds_updates"]) == 0 for row in sample.values()), "static scene rebuilds world boxes"
    metrics = {"gpu_frame_ms": metric([float(gpu_map[index]["gpu_frame_ms"]) for index in sample]),
               "gpu_geometry_ms": metric([float(gpu_map[index]["gpu_geometry_ms"]) for index in sample]),
               "wall_delta_ms": metric([float(row["raw_delta_ms"]) for row in sample.values()])}
    for scope in ("CPU.Frame", "CPU.CommandPrepare", "CPU.CommandRecord", "CPU.SubmitPresent", "CPU.Extract", "CPU.BenchmarkCollect"):
        values = [cpu_map[index][scope] for index in sample if scope in cpu_map[index]]
        assert len(values) == len(sample), scope
        metrics[scope] = metric(values)
    if run["culling"]:
        metrics["CPU.Visibility"] = metric([cpu_map[index]["CPU.Visibility"] for index in sample])
    if run["sorting"]:
        metrics["CPU.OpaqueSort"] = metric([cpu_map[index]["CPU.OpaqueSort"] for index in sample])
    if run["instancing"]:
        metrics["CPU.Instancing"] = metric([cpu_map[index]["CPU.Instancing"] for index in sample])
    bindings = {key: metric([int(row[key]) for row in sample.values()]) for key in
                ("opaque_pipeline_binds", "opaque_root_binds", "opaque_table_binds", "opaque_vertex_binds", "opaque_index_binds", "opaque_constant_binds") if key in frames[0]}
    return {"run": directory.name, "scene": run["scene"], "object_count": run["object_count"],
            "instancing": run["instancing"],
            "instance_bytes": metric([int(row.get("instance_bytes", 0)) for row in sample.values()]),
            "draw_constant_bytes": metric([int(row.get("draw_constant_bytes", 0)) for row in sample.values()]),
            "upload_reserved_bytes": metric([int(row.get("upload_reserved_bytes", 0)) for row in sample.values()]),
            "sorting": run["sorting"], "binding_cache": run.get("binding_cache", False), "opaque_bindings": bindings,
            "visibility_case": run.get("visibility_case", "baseline"), "culling": run["culling"],
            "draws": metric([int(row["draws"]) for row in sample.values()]),
            "culled": metric([int(row.get("culled", 0)) for row in sample.values()]),
            "layout_sha256": run["layout_sha256"], "sample_frames": len(sample), "all_frames": len(frames),
            "sample_first_s": min(times), "sample_last_s": max(times), "elapsed_s": run["elapsed_seconds"],
            "adapter": run["adapter"], "cpu_brand": run["cpu_brand"], "ac_line_status": run["ac_line_status"],
            "sample_occluded_frames": sum(int(row["occluded"]) for row in sample.values()),
            "memory_used_bytes": int(next(iter(sample.values()))["memory_used_bytes"]), "metrics": metrics}

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--formal", action="store_true")
    args = parser.parse_args()
    environment = json.loads((args.input / "environment.json").read_text(encoding="utf-8"))
    runs = [validate(path.parent, environment, args.formal) for path in sorted(args.input.glob("*/run.json"))]
    assert all(run["visibility_case"] == "baseline" and not run["culling"] and not run["sorting"] and not run["binding_cache"] for run in runs), "use optimization comparison analyzer for variants"
    assert {run["scene"] for run in runs} == {"repeated", "mixed"}
    variation = {}
    for scene in ("repeated", "mixed"):
        group = [run for run in runs if run["scene"] == scene]
        assert len(group) == environment["repetitions"] and len({run["layout_sha256"] for run in group}) == 1
        if args.formal:
            assert len(group) >= 3
        values = [run["metrics"]["gpu_frame_ms"]["median"] for run in group]
        mean = statistics.mean(values)
        variation[scene] = {"gpu_medians_ms": values, "mean": mean,
                            "coefficient_of_variation": statistics.stdev(values) / mean if len(values) > 1 and mean else None}
    result = {"schema": 1, "formal_protocol_passed": args.formal, "quantiles": "sample median; p95/p99 nearest-rank ceil(p*N)",
              "source_sha256": environment["source_sha256"], "environment": environment, "runs": runs, "variation": variation}
    (args.input / "summary.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    lines = ["# 压力测试基线", "", f"正式协议通过：{args.formal}。源码摘要 `{environment['source_sha256']}`。", "",
             "所有数字来自各目录原始 CSV；预热与 tail 已排除，分位数采用 nearest-rank。CPU scope 包含子区间，不能相加；GPU 也不能与 CPU 相加。", "",
             "| 场景/轮次 | 样本帧 | GPU 中位/P95/P99 ms | CPU 准备中位 ms | API used 字节 |", "|---|---:|---|---:|---:|"]
    for run in runs:
        gpu = run["metrics"]["gpu_frame_ms"]
        lines.append(f"| {run['run']} | {run['sample_frames']} | {gpu['median']:.6f} / {gpu['p95']:.6f} / {gpu['p99']:.6f} | {run['metrics']['CPU.CommandPrepare']['median']:.6f} | {run['memory_used_bytes']} |")
    lines += ["", "重复测量 GPU 中位数变异系数："]
    for scene, values in variation.items():
        cv = values["coefficient_of_variation"]
        description = f"{cv:.4%}" if cv is not None else "样本不足（少于两轮）"
        lines.append(f"- {scene}: {description}，各轮值见 summary.json。")
    lines += ["", "Present sync interval 为 1。隐藏窗口可能被标为 occluded，应用有相应等待；wall delta/FPS 不能当作纯渲染成本。逐帧状态、真实相机和 CPU.BenchmarkCollect 开销保留。", "",
              "温度/功耗仅为 sensors.json 的起止观测，不代表峰值或频率锁定。电源、驱动、OS 见 environment.json/run.json；普通桌面后台负载未隔离，不改变电源计划，也不据此承诺任何对象数量稳定 FPS。"]
    (args.input / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    files = [{"path": str(path.relative_to(args.input)).replace("\\", "/"), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
             for path in sorted(args.input.rglob("*")) if path.is_file() and path.name != "artifact-digests.json"]
    (args.input / "artifact-digests.json").write_text(json.dumps(files, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Validated {len(runs)} runs; formal={args.formal}; raw CPU/GPU frame pairing complete.")

if __name__ == "__main__":
    main()
