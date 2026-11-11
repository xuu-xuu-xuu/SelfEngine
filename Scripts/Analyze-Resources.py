"""核对真实资源对象/物理退休、高水位、单变量控制与固定循环性能。"""
from pathlib import Path
import argparse
import csv
import hashlib
import importlib.util
import json
import math
import statistics

spec = importlib.util.spec_from_file_location("benchmark_analysis", Path(__file__).with_name("Analyze-Benchmarks.py"))
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)

CASES = {"baseline": ("placed", "batch", True), "committed": ("committed", "batch", True),
         "individual": ("placed", "individual", True), "uncached": ("placed", "batch", False)}


def validate(directory, environment, formal):
    run = json.loads((directory / "run.json").read_text(encoding="utf-8"))
    assert run["success"] and run["measurement_complete"] and run["mode"] == "measurement" and run["configuration"] == "Release"
    assert run["source_sha256"] == environment["source_sha256"] and run["executable_sha256"] == environment["executable_sha256"]
    label = directory.name.rsplit("-", 1)[0]
    assert (run["memory"], run["upload"], run["pso_cache"]) == CASES[label]
    if formal:
        assert not run["light"] and not environment["smoke"] and run["cycles"] >= 24 and run["warmup_cycles"] >= 4
        assert (run["texture_count"], run["mesh_count"], run["pipeline_requests"], run["texture_size"]) == (24, 6, 16, [257, 193])
    rows = baseline.read_csv(directory / "cycles.csv")
    cpu, gpu = baseline.read_csv(directory / "cpu.csv"), baseline.read_csv(directory / "gpu.csv")
    assert len(rows) == run["total_cycles"] == run["cycles"] + run["warmup_cycles"] == len(gpu)
    gpu_map = {int(row["source_frame"]): row for row in gpu}
    assert len(gpu_map) == len(gpu) and set(gpu_map) == set(range(1, run["total_cycles"] + 1))
    cpu_map, wait_calls = {}, {}
    for row in cpu:
        scopes = cpu_map.setdefault(int(row["frame"]), {})
        assert row["scope"] not in scopes
        scopes[row["scope"]] = float(row["cpu_ms"])
        if row["scope"] == "CPU.GpuFenceWait":
            wait_calls[int(row["frame"])] = int(row["calls"])
    assert gpu_map.keys() == cpu_map.keys() == {int(row["cycle"]) for row in rows}
    base = run["snapshots"]
    for row in rows:
        cycle = int(row["cycle"])
        assert row["phase"] == ("warmup" if cycle <= run["warmup_cycles"] else "sample")
        assert "CPU.Frame" in cpu_map[cycle] and "CPU.ResourceLoad" in cpu_map[cycle] and "CPU.ResourceRetire" in cpu_map[cycle]
        assert int(row["loaded_buffers"]) == run["mesh_count"] * 2 and int(row["loaded_textures"]) == run["texture_count"]
        assert int(row["loaded_pipelines"]) == run["pipeline_requests"] and int(row["loaded_live_prepared"]) == (1 if run["pso_cache"] else run["pipeline_requests"])
        assert int(row["pso_creations"]) == (1 if run["pso_cache"] else run["pipeline_requests"])
        assert int(row["pso_hits"]) == (run["pipeline_requests"] - 1 if run["pso_cache"] else 0)
        assert int(row["upload_submissions"]) == (1 if run["upload"] == "batch" else run["mesh_count"] * 2 + run["texture_count"])
        for key in ("buffers", "textures", "pipelines", "memory_resources", "memory_used", "srv_used"):
            assert int(row["drained_" + key]) == base["baseline_" + key]
        assert all(int(row["drained_" + key]) == 0 for key in ("retiring", "in_flight", "live_prepared", "live_programs"))
        assert all(int(row["drop_" + key]) == 0 for key in ("buffers", "textures", "pipelines"))
        assert int(row["debug_errors"]) == int(row["debug_warnings"]) == int(row["shader_compilations"]) == 0
        assert int(gpu_map[cycle]["draws"]) == run["texture_count"] and float(gpu_map[cycle]["gpu_frame_ms"]) >= 0
    sample = [row for row in rows if row["phase"] == "sample"]
    assert len(sample) == run["cycles"]
    for key in ("drained_memory_reserved", "drained_srv_capacity", "drained_frame_capacity", "drained_upload_reserved", "drained_upload_pages", "drained_tracked_resources"):
        assert len({row[key] for row in sample}) == 1, (directory.name, "unbounded post-warmup state", key)
    if run["memory"] == "placed":
        assert all(int(row["heap_creations"]) == int(row["committed_creations"]) == 0 for row in sample), "pool backing keeps growing"
    else:
        assert all(int(row["committed_creations"]) == run["mesh_count"] * 2 + run["texture_count"] for row in sample)
    assert base["trimmed_memory_used"] == base["baseline_memory_used"] and base["trimmed_memory_reserved"] == base["baseline_memory_reserved"]
    metrics = {name: baseline.metric([float(row[name]) for row in sample]) for name in ("load_calls_ms", "load_ready_ms", "draw_submit_ms", "retire_ms")}
    metrics["gpu_frame_ms"] = baseline.metric([float(gpu_map[int(row["cycle"])]["gpu_frame_ms"]) for row in sample])
    metrics["CPU.Frame"] = baseline.metric([cpu_map[int(row["cycle"])]["CPU.Frame"] for row in sample])
    waits = []
    for row in sample:
        waits.append(cpu_map[int(row["cycle"])].get("CPU.GpuFenceWait", 0))
    metrics["CPU.GpuFenceWait"] = baseline.metric(waits)
    metrics["blocking_fence_wait_calls"] = baseline.metric([wait_calls.get(int(row["cycle"]), 0) for row in sample])
    return {"run": directory.name, "case": label, "workload_sha256": run["workload_sha256"], "cycles": len(sample), "metrics": metrics,
            "upload_submissions": int(sample[0]["upload_submissions"]), "pso_creations": int(sample[0]["pso_creations"]),
            "loaded_memory_used": int(sample[0]["loaded_memory_used"]), "loaded_memory_reserved": int(sample[0]["loaded_memory_reserved"]),
            "drained_memory_reserved": int(sample[0]["drained_memory_reserved"]), "upload_reserved": int(sample[0]["drained_upload_reserved"]),
            "load_global_waits": baseline.metric([int(row["load_global_waits"]) for row in sample])}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--formal", action="store_true")
    args = parser.parse_args()
    environment = json.loads((args.input / "environment.json").read_text(encoding="utf-8"))
    runs = [validate(path.parent, environment, args.formal) for path in sorted(args.input.glob("*/run.json"))]
    assert len(runs) == environment["repetitions"] * 4 and len({run["workload_sha256"] for run in runs}) == 1
    images, capture_workloads = [], []
    for label in CASES:
        capture = json.loads((args.input / "captures" / label / "run.json").read_text(encoding="utf-8"))
        assert capture["success"] and capture["mode"] == "capture" and not capture["measurement_complete"]
        assert capture["source_sha256"] == environment["source_sha256"] and capture["executable_sha256"] == environment["executable_sha256"]
        assert (capture["memory"], capture["upload"], capture["pso_cache"]) == CASES[label]
        assert not capture["light"] and (capture["texture_count"], capture["mesh_count"], capture["pipeline_requests"], capture["texture_size"]) == (24, 6, 16, [257, 193])
        assert capture["total_cycles"] == 1 and capture["debug_errors"] == capture["debug_warnings"] == 0
        capture_workloads.append(capture["workload_sha256"])
        images.append((args.input / "captures" / (label + ".ppm")).read_bytes())
    assert all(image == images[0] for image in images), "resource policy changed image"
    assert len(set(capture_workloads)) == 1 and (environment["smoke"] or capture_workloads[0] == runs[0]["workload_sha256"])
    groups = {}
    for label in CASES:
        group = [run for run in runs if run["case"] == label]
        assert len(group) == environment["repetitions"] and (not args.formal or len(group) >= 3)
        medians = [run["metrics"]["load_ready_ms"]["median"] for run in group]
        groups[label] = {"load_ready_ms": statistics.mean(medians), "load_ready_median_cv": statistics.stdev(medians) / statistics.mean(medians) if len(medians) > 1 else None,
            "draw_ms": statistics.mean(run["metrics"]["draw_submit_ms"]["median"] for run in group), "retire_ms": statistics.mean(run["metrics"]["retire_ms"]["median"] for run in group),
            "uploads": group[0]["upload_submissions"], "pso_creates": group[0]["pso_creations"], "loaded_reserved": group[0]["loaded_memory_reserved"], "drained_reserved": group[0]["drained_memory_reserved"],
            "global_load_waits": statistics.mean(run["load_global_waits"]["median"] for run in group),
            "blocking_wait_calls": statistics.mean(run["metrics"]["blocking_fence_wait_calls"]["median"] for run in group),
            "blocking_wait_ms": statistics.mean(run["metrics"]["CPU.GpuFenceWait"]["median"] for run in group)}
    result = {"schema": 1, "formal_protocol_passed": args.formal, "environment": environment, "runs": runs, "groups": groups, "whole_image_sha256": hashlib.sha256(images[0]).hexdigest()}
    (args.input / "summary.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    lines = ["# 资源生命周期单变量对照", "", f"正式协议通过：{args.formal}；源 `{environment['source_sha256']}`。", "",
             ("短烟雾采用小集合，不是正式性能结论。完整集合采图另起进程。" if environment["smoke"] else
              "固定 24 张图/6 VB-IB 组/16 等效管线请求；各路三次，每次 4 循环预热+24 样本循环。这是固定操作延迟实验，不是 60 秒稳态 FPS。"), "",
             "| 路径 | load ready ms | draw submit ms | retire ms | 阻塞 Fence ms/次 | 上传提交/循环 | Native PSO 创建/循环 | load 全局等待 | DEFAULT loaded/retired reserved 字节 |", "|---|---:|---:|---:|---:|---:|---:|---:|---|"]
    for label, item in groups.items():
        lines.append(f"| {label} | {item['load_ready_ms']:.6f} | {item['draw_ms']:.6f} | {item['retire_ms']:.6f} | {item['blocking_wait_ms']:.6f}/{item['blocking_wait_calls']:.1f} | {item['uploads']} | {item['pso_creates']} | {item['global_load_waits']:.0f} | {item['loaded_reserved']}/{item['drained_reserved']} |")
    lines += ["", "所有循环释放逻辑句柄后再 drain；Fence 完成后 retiring/in-flight、逻辑 Buffer/Texture/Pipeline、存活 prepared/program 归零，本轮物理 used/resources 与窗口基线相同。CPU SRV used 归基线，Frame 可见表、UploadArena 与空 placed 页可保留容量；预热后指标稳定，末尾 Trim 空页回到基线。", "",
              "individual 每次资源上传后检查其 Fence，batch 只提交一次后统一等待。CPU.GpuFenceWait 只记录检查时尚未完成而进入阻塞路径的次数/耗时，包含本循环加载、Draw/退休等待，不等于上传数量；load 全局等待仅计显式 flush，两路都执行一次。PSO cache 只影响 prepared 查找/插入，Shader program 缓存不变；弱缓存在轮末退休，下轮重新创建是预期行为。没有依据不完整 ETW 做总时序结论。", "",
              "CPU scopes 有嵌套，不能相加。API used/reserved 不是系统全部显存，保留量与分配请求次数不是同一指标；内存池可能以更多保留空间换请求复用。普通桌面未隔离，温度/功耗仅起止、未改电源频率，不据此保证所有工作负载更快。"]
    (args.input / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    digests = [{"path": str(path.relative_to(args.input)).replace("\\", "/"), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
               for path in sorted(args.input.rglob("*")) if path.is_file() and path.name != "artifact-digests.json"]
    (args.input / "artifact-digests.json").write_text(json.dumps(digests, indent=2) + "\n", encoding="utf-8")
    print(f"Validated {len(runs)} resource runs; formal={args.formal}; retirement/Trim/high-water and full images verified.")


if __name__ == "__main__":
    main()
