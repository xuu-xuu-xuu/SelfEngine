"""固定控制项，核对实例化的真实 Draw、数据字节、全图和十二轮 CPU/GPU 对照。"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import statistics

spec = importlib.util.spec_from_file_location("benchmark_analysis", Path(__file__).with_name("Analyze-Benchmarks.py"))
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)


def expected_batches(scene, count, seed, outside=False):
    if scene == "repeated":
        return 1
    state, groups = seed, set()
    for index in range(count):
        for _ in range(6):
            state = (state ^ (state << 13)) & 0xffffffff
            state ^= state >> 17
            state = (state ^ (state << 5)) & 0xffffffff
        if not outside or index % 10 == 0:
            groups.add(((state >> 1) % 24, state & 1))
    return len(groups)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--formal", action="store_true")
    args = parser.parse_args()
    environment = json.loads((args.input / "environment.json").read_text(encoding="utf-8"))
    assert environment["suite"] == "Instancing"
    runs = [baseline.validate(path.parent, environment, args.formal) for path in sorted(args.input.glob("*/run.json"))]
    assert len(runs) == environment["repetitions"] * 4
    images, comparisons = {}, {}
    for case in ("repeated", "mixed", "mostly_outside"):
        pair = []
        for enabled in (False, True):
            name = case + ("-on" if enabled else "-off")
            directory = args.input / "captures" / name
            shot = json.loads((directory / "run.json").read_text(encoding="utf-8"))
            assert shot["success"] and shot["mode"] == "capture" and not shot["measurement_complete"]
            assert not shot["sorting"] and shot["binding_cache"] and shot["instancing"] == enabled
            assert shot["culling"] == (case == "mostly_outside") and shot["source_sha256"] == environment["source_sha256"]
            assert shot["executable_sha256"] == environment["executable_sha256"]
            frames = baseline.read_csv(directory / "frames.csv")
            expected = expected_batches(shot["scene"], shot["object_count"], shot["seed"], case == "mostly_outside") if enabled else (100 if case == "mostly_outside" else 1000)
            assert all(int(row["draws"]) == expected for row in frames)
            pair.append((args.input / "captures" / (name + ".ppm")).read_bytes())
        assert pair[0] == pair[1], "instancing changed full GPU image: " + case
        images[case] = hashlib.sha256(pair[0]).hexdigest()
    for scene in ("repeated", "mixed"):
        sides = {}
        for enabled in (False, True):
            group = [run for run in runs if run["scene"] == scene and run["instancing"] == enabled]
            assert len(group) == environment["repetitions"] and (not args.formal or len(group) >= 3)
            assert len({run["layout_sha256"] for run in group}) == 1
            for run in group:
                assert not run["sorting"] and run["binding_cache"] and not run["culling"]
                metadata = json.loads((args.input / run["run"] / "run.json").read_text(encoding="utf-8"))
                expected = expected_batches(scene, run["object_count"], metadata["seed"]) if enabled else run["object_count"]
                assert run["draws"]["median"] == run["draws"]["p95"] == run["draws"]["p99"] == expected
                assert run["instance_bytes"]["median"] == (run["object_count"] * 240 if enabled else 0)
                assert run["draw_constant_bytes"]["median"] == expected * 512
            gpu = [run["metrics"]["gpu_frame_ms"]["median"] for run in group]
            sides["on" if enabled else "off"] = {"runs": [run["run"] for run in group], "layout_sha256": group[0]["layout_sha256"],
                "draws": group[0]["draws"]["median"], "instance_bytes": group[0]["instance_bytes"]["median"],
                "draw_constant_bytes": group[0]["draw_constant_bytes"]["median"], "upload_reserved_bytes": group[0]["upload_reserved_bytes"]["median"],
                "prepare_ms": statistics.mean(run["metrics"]["CPU.CommandPrepare"]["median"] for run in group),
                "batch_ms": statistics.mean(run["metrics"].get("CPU.Instancing", {"median": 0})["median"] for run in group),
                "record_ms": statistics.mean(run["metrics"]["CPU.CommandRecord"]["median"] for run in group),
                "frame_ms": statistics.mean(run["metrics"]["CPU.Frame"]["median"] for run in group),
                "gpu_ms": statistics.mean(gpu), "gpu_median_cv": statistics.stdev(gpu) / statistics.mean(gpu) if len(gpu) > 1 else None}
        assert sides["on"]["layout_sha256"] == sides["off"]["layout_sha256"]
        sides["frame_change_percent"] = (sides["on"]["frame_ms"] / sides["off"]["frame_ms"] - 1) * 100
        sides["gpu_change_percent"] = (sides["on"]["gpu_ms"] / sides["off"]["gpu_ms"] - 1) * 100
        comparisons[scene] = sides
    result = {"schema": 1, "formal_protocol_passed": args.formal, "environment": environment, "runs": runs, "comparisons": comparisons, "images": images}
    (args.input / "summary.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    lines = ["# PBR 实例化对照", "", f"正式协议通过：{args.formal}；源 `{environment['source_sha256']}`。", "",
             "固定 sorting=false、binding_cache=true，只改变 instancing。每行是各轮中位数的算术均值；完整 P95/P99、CV 与原始数据见 summary.json。", "",
             "| 场景 | 实例化 | Draw | CPU 准备 ms | 其中合批 ms | CPU 录制 ms | CPU Frame ms | GPU ms |", "|---|---|---:|---:|---:|---:|---:|---:|"]
    for scene, values in comparisons.items():
        for label in ("off", "on"):
            item = values[label]
            lines.append(f"| {scene} | {label} | {item['draws']:.0f} | {item['prepare_ms']:.6f} | {item['batch_ms']:.6f} | {item['record_ms']:.6f} | {item['frame_ms']:.6f} | {item['gpu_ms']:.6f} |")
    lines += ["", "| 场景/开关 | 实例数据/帧 字节 | Draw CBV/帧 字节 | 两 FrameContext 上传 arena 保留字节 |", "|---|---:|---:|---:|"]
    for scene, values in comparisons.items():
        for label in ("off", "on"):
            item = values[label]
            lines.append(f"| {scene}/{label} | {item['instance_bytes']:.0f} | {item['draw_constant_bytes']:.0f} | {item['upload_reserved_bytes']:.0f} |")
    for scene, values in comparisons.items():
        lines += ["", f"{scene}: CPU Frame {values['frame_change_percent']:+.2f}%，GPU {values['gpu_change_percent']:+.2f}%。"]
    lines += ["", "InstanceGpuData 每条 240 字节；CBV 512 对齐，arena 保留包含对齐/页和两帧高水位，不是系统全部显存。CPU 容量与临时批次内存不计入这些 GPU 字节。",
              "CPU.Instancing 已计入准备，Frame 已含子区间，不能相加；单例也走实例管线。重复/混合/剔除后 100 对象的完整开关图均一致。", "",
              "当前实例管线仅用于 PBR/Benchmark，不宣称 HDR/Shadow/Environment 已支持。Present interval=1，occluded/raw delta 保留，wall FPS 不当作纯 GPU/CPU 吞吐；桌面负载未隔离，温度/功耗仅起止观测，没有修改电源或频率。"]
    (args.input / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    digests = [{"path": str(path.relative_to(args.input)).replace("\\", "/"), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
               for path in sorted(args.input.rglob("*")) if path.is_file() and path.name != "artifact-digests.json"]
    (args.input / "artifact-digests.json").write_text(json.dumps(digests, indent=2) + "\n", encoding="utf-8")
    print(f"Validated instancing comparison: {len(runs)} runs; formal={args.formal}; all three image pairs identical.")


if __name__ == "__main__":
    main()
