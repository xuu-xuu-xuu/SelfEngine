"""核对固定运行帧、上传硬预算与真实完成，再比较加载时间和长尾。"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import statistics

spec = importlib.util.spec_from_file_location("base", Path(__file__).with_name("Analyze-Benchmarks.py"))
base = importlib.util.module_from_spec(spec)
spec.loader.exec_module(base)


def validate(directory, environment, formal):
    run = json.loads((directory / "run.json").read_text(encoding="utf-8"))
    assert run["success"] and run["measurement_complete"] and run["mode"] == "measurement" and run["configuration"] == "Release"
    assert run["source_sha256"] == environment["source_sha256"] and run["executable_sha256"] == environment["executable_sha256"]
    assert not run["modified"] and not run["interrupted"] and run["epochs"] == 1 and run["cancel_frame"] == run["restart_frame"] == 0
    if formal:
        assert not run["light"] and not environment["smoke"] and (run["objects"], run["texture_size"], run["frames"], run["trigger_frame"], run["frame_interval_ms"]) == (16, [1025, 513], 480, 30, 16)
        assert (run["budget_bytes"], run["budget_tasks"]) == (262144, 4)
    assert run["baseline_memory_used"] == run["final_memory_used"] and run["baseline_memory_resources"] == run["final_memory_resources"] and run["baseline_srv_used"] == run["final_srv_used"]
    assert run["debug_warnings"] == run["debug_errors"] == run["final_retiring"] == run["final_in_flight"] == 0 and run["logical_textures_after_shutdown"] == 1
    frames, cpu, gpu = (base.read_csv(directory / name) for name in ("frames.csv", "cpu.csv", "gpu.csv"))
    frame_map = {int(row["frame"]): row for row in frames}
    gpu_map = {int(row["source_frame"]): row for row in gpu}
    assert len(frame_map) == len(frames) == len(gpu_map) == len(gpu) == run["frames"]
    assert frame_map.keys() == gpu_map.keys() == set(range(1, run["frames"] + 1))
    cpu_map = {}
    wait_calls = {}
    for row in cpu:
        scopes = cpu_map.setdefault(int(row["frame"]), {})
        assert row["scope"] not in scopes
        scopes[row["scope"]] = float(row["cpu_ms"])
        if row["scope"] == "CPU.GpuFenceWait":
            wait_calls[int(row["frame"])] = int(row["calls"])
    assert cpu_map.keys() == frame_map.keys() and all("CPU.Frame" in row for row in cpu_map.values())
    logical = run["objects"] * run["texture_size"][0] * run["texture_size"][1] * 4
    assert run["upload_payload_bytes"] == logical and sum(int(row["device_upload_payload"]) for row in frames) == logical
    ready = [int(row["ready"]) for row in frames]
    assert ready == sorted(ready) and ready[-1] == run["objects"] and ready[0] == 0
    first_ready = next(int(row["frame"]) for row in frames if int(row["ready"]) == run["objects"])
    assert first_ready == run["all_ready_frame"] >= run["trigger_frame"] and run["total_ready_ms"] > 0
    async_mode = run["strategy"] == "async"
    for row in frames:
        frame = int(row["frame"])
        assert row["phase"] == ("warmup" if frame < run["trigger_frame"] else "loading_window")
        assert int(row["draws"]) == int(gpu_map[frame]["draws"]) == run["objects"]
        assert int(row["pipeline_creations"]) == int(row["shader_compilations"]) == int(row["debug_warnings"]) == int(row["debug_errors"]) == 0
        assert int(row["native_staged_footprint"]) == int(row["staged_footprint_bytes"]) and int(row["native_staged_copies"]) == int(row["copy_tasks"])
        if async_mode:
            assert int(row["staged_footprint_bytes"]) <= run["budget_bytes"] and int(row["copy_tasks"]) <= run["budget_tasks"] and int(row["global_waits"]) == 0
        else:
            assert int(row["copy_tasks"]) == 0
    if async_mode:
        assert sum(int(row["staged_payload_bytes"]) for row in frames) == logical and run["all_ready_frame"] > run["trigger_frame"]
        assert sum(int(row["upload_submissions"]) > 0 for row in frames) > 1
    else:
        assert run["all_ready_frame"] == run["trigger_frame"] and int(frame_map[run["trigger_frame"]]["upload_submissions"]) == run["objects"]
    indices = range(run["trigger_frame"], run["frames"] + 1)
    metrics = {name: base.metric([cpu_map[index].get(name, 0) for index in indices]) for name in ("CPU.Frame", "CPU.AssetUpload", "CPU.LoadingSynchronous", "CPU.GpuFenceWait")}
    metrics["draw_gpu_ms"] = base.metric([float(gpu_map[index]["gpu_frame_ms"]) for index in indices])
    metrics["raw_delta_ms"] = base.metric([float(frame_map[index]["raw_delta_ms"]) for index in indices])
    worst = max(indices, key=lambda index: cpu_map[index]["CPU.Frame"])
    return {"run": directory.name, "strategy": run["strategy"], "workload_sha256": run["workload_sha256"], "metrics": metrics, "total_ready_ms": run["total_ready_ms"], "all_ready_frame": first_ready,
            "cpu_worst_ms": cpu_map[worst]["CPU.Frame"], "cpu_worst_frame": worst, "cpu_over_16_67_ms": sum(cpu_map[index]["CPU.Frame"] > 1000 / 60 for index in indices),
            "max_upload_footprint": max(int(row["staged_footprint_bytes"]) for row in frames), "copies": sum(int(row["copy_tasks"]) for row in frames),
            "blocking_fence_calls": sum(wait_calls.get(index, 0) for index in indices), "frames": len(frames), "sample_frames": len(indices)}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--formal", action="store_true")
    args = parser.parse_args()
    environment = json.loads((args.input / "environment.json").read_text(encoding="utf-8"))
    runs = [validate(path.parent, environment, args.formal) for path in sorted(args.input.glob("*/run.json"))]
    assert len(runs) == environment["repetitions"] * 2 and len({run["workload_sha256"] for run in runs}) == 1
    images = []
    for strategy in ("sync", "async"):
        capture = json.loads((args.input / "captures" / strategy / "run.json").read_text(encoding="utf-8"))
        assert capture["success"] and capture["mode"] == "capture" and not capture["measurement_complete"] and capture["strategy"] == strategy
        assert capture["source_sha256"] == environment["source_sha256"] and capture["executable_sha256"] == environment["executable_sha256"] and capture["workload_sha256"] == runs[0]["workload_sha256"]
        assert capture["debug_warnings"] == capture["debug_errors"] == 0
        images.append((args.input / "captures" / (strategy + ".ppm")).read_bytes())
    assert images[0] == images[1], "final complete loading GPU images differ"
    groups = {}
    for strategy in ("sync", "async"):
        group = [run for run in runs if run["strategy"] == strategy]
        assert len(group) == environment["repetitions"] and (not args.formal or len(group) >= 3)
        totals = [run["total_ready_ms"] for run in group]
        worst = [run["cpu_worst_ms"] for run in group]
        groups[strategy] = {"ready_ms": statistics.mean(totals), "ready_cv": statistics.stdev(totals) / statistics.mean(totals) if len(totals) > 1 else None,
            "p95_ms": statistics.mean(run["metrics"]["CPU.Frame"]["p95"] for run in group), "p99_ms": statistics.mean(run["metrics"]["CPU.Frame"]["p99"] for run in group),
            "worst_ms": statistics.mean(worst), "worst_cv": statistics.stdev(worst) / statistics.mean(worst) if len(worst) > 1 else None,
            "over_16_67": [run["cpu_over_16_67_ms"] for run in group], "all_ready_frames": [run["all_ready_frame"] for run in group]}
    result = {"schema": 1, "formal_protocol_passed": args.formal, "environment": environment, "runs": runs, "groups": groups, "whole_image_sha256": hashlib.sha256(images[0]).hexdigest()}
    (args.input / "summary.json").write_text(json.dumps(result, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    lines = ["# 运行中资源加载对照", "", f"正式协议通过：{args.formal}；源 `{environment['source_sha256']}`。", "",
             "同一固定输入、World/相机/动画和触发帧；源文件在计时前逐个读取校验，文件缓存因此预热，不是冷存储测试。同步在触发帧读取解码并逐资源上传；后台复用解码/缓存、CPU 队列和每帧预算，加载路径不同的成本全部保留。截图独立进程，不进入性能样本。", "",
             "| 模式 | 总就绪 ms 均值 | CPU P95 ms | CPU P99 ms | 最差 CPU 帧 ms 均值 | CPU >16.67 ms 帧数 | 就绪帧 |", "|---|---:|---:|---:|---:|---|---|"]
    for strategy, item in groups.items():
        lines.append(f"| {strategy} | {item['ready_ms']:.3f} | {item['p95_ms']:.6f} | {item['p99_ms']:.6f} | {item['worst_ms']:.6f} | {item['over_16_67']} | {item['all_ready_frames']} |")
    lines += ["", "CPU 样本窗口从触发帧开始，包含同步加载长帧与后续完整固定窗口，分位数使用 nearest-rank；单次卡顿可能不进入 P99，因此另报最差帧和超阈值数。CPU scopes 嵌套不可相加。总就绪时间含后台预算跨帧与应用限帧，不等于纯复制耗时。", "",
              "每帧限帧 sleep 在 CPU.Frame 外，raw delta 含 sleep/Present/等待，不作为吞吐。GPU timestamp 只覆盖 Draw 提交，不含独立上传批次，不报告伪造的上传 GPU 毫秒。后台硬上限按实际 padded staging 字节/复制调用核对，行距/放置对齐和逻辑 payload 分列；自身帧上下文必要 Fence 等待仍计入。", "",
              "两模式最终完整图同字节，退出 drain 后对象/CPU SRV used 回到场景常驻基线；UploadArena/空页保留成本不强行清零。普通桌面未隔离，温度/频率仅起止，电源不改；该结果不承诺零卡顿或所有场景更快。"]
    (args.input / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    digests = [{"path": str(path.relative_to(args.input)).replace("\\", "/"), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
               for path in sorted(args.input.rglob("*")) if path.is_file() and path.name != "artifact-digests.json"]
    (args.input / "artifact-digests.json").write_text(json.dumps(digests, indent=2) + "\n", encoding="utf-8")
    print(f"Validated {len(runs)} loading runs; formal={args.formal}; complete paired frames, hard budgets, retirement and full images passed.")


if __name__ == "__main__":
    main()
