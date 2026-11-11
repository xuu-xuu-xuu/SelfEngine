"""核对生产渲染器的视锥开关画面、计数、固定盒缓存及十二轮对照。"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import statistics

spec = importlib.util.spec_from_file_location("benchmark_analysis", Path(__file__).with_name("Analyze-Benchmarks.py"))
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--formal", action="store_true")
    args = parser.parse_args()
    environment = json.loads((args.input / "environment.json").read_text(encoding="utf-8"))
    assert environment["suite"] == "Culling"
    runs = [baseline.validate(path.parent, environment, args.formal) for path in sorted(args.input.glob("*/run.json"))]
    comparisons = {}
    captures = args.input / "captures"
    for case in ("all_visible", "mostly_outside"):
        images = []
        for enabled in (False, True):
            label = case + ("-on" if enabled else "-off")
            run = json.loads((captures / label / "run.json").read_text(encoding="utf-8"))
            assert run["success"] and run["mode"] == "capture" and not run["measurement_complete"]
            assert run["culling"] == enabled and run["visibility_case"] == case
            assert run["source_sha256"] == environment["source_sha256"] and run["executable_sha256"] == environment["executable_sha256"]
            images.append((captures / (label + ".ppm")).read_bytes())
        assert images[0] == images[1], "culling changed full GPU image: " + case
        sides = {}
        for enabled in (False, True):
            group = [run for run in runs if run["visibility_case"] == case and run["culling"] == enabled]
            assert len(group) == environment["repetitions"] and (not args.formal or len(group) >= 3)
            assert len({run["layout_sha256"] for run in group}) == 1
            sides["on" if enabled else "off"] = {"runs": [run["run"] for run in group], "layout_sha256": group[0]["layout_sha256"],
                "gpu_ms": statistics.mean(run["metrics"]["gpu_frame_ms"]["median"] for run in group),
                "prepare_ms": statistics.mean(run["metrics"]["CPU.CommandPrepare"]["median"] for run in group),
                "visibility_ms": statistics.mean(run["metrics"].get("CPU.Visibility", {"median": 0})["median"] for run in group),
                "draws": statistics.mean(run["draws"]["median"] for run in group)}
            values = [run["metrics"]["gpu_frame_ms"]["median"] for run in group]
            sides["on" if enabled else "off"]["gpu_median_cv"] = statistics.stdev(values) / statistics.mean(values) if len(values) > 1 else None
        assert sides["on"]["layout_sha256"] == sides["off"]["layout_sha256"]
        sides["prepare_change_percent"] = (sides["on"]["prepare_ms"] / sides["off"]["prepare_ms"] - 1) * 100
        sides["gpu_change_percent"] = (sides["on"]["gpu_ms"] / sides["off"]["gpu_ms"] - 1) * 100
        sides["image_sha256"] = hashlib.sha256(images[0]).hexdigest()
        comparisons[case] = sides
    assert len(runs) == environment["repetitions"] * 4
    result = {"schema": 1, "formal_protocol_passed": args.formal, "environment": environment, "runs": runs, "comparisons": comparisons}
    (args.input / "summary.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    lines = ["# CPU 视锥剔除对照", "", f"正式协议通过：{args.formal}；源码 `{environment['source_sha256']}`。", "",
             "每组均值为各独立轮次中位数的算术均值；完整 P95/P99、每轮数据和 CV 见 summary.json。百分比负数表示减少，不能把 CPU 嵌套区间相加。", "",
             "| 布局 | 开关 | Draw | CPU 准备 ms | 其中可见性 ms | GPU ms |", "|---|---|---:|---:|---:|---:|"]
    for case, values in comparisons.items():
        for label in ("off", "on"):
            item = values[label]
            lines.append(f"| {case} | {label} | {item['draws']:.0f} | {item['prepare_ms']:.6f} | {item['visibility_ms']:.6f} | {item['gpu_ms']:.6f} |")
    for case, values in comparisons.items():
        lines += ["", f"{case}: CPU 准备变化 {values['prepare_change_percent']:+.2f}%，GPU 变化 {values['gpu_change_percent']:+.2f}%；开关完整图 SHA-256 `{values['image_sha256']}` 一致。", ""]
    lines += ["静态对象采样窗口世界盒更新均为 0；每帧仍包含 O(N) 哈希查询与六平面测试。全可见时此项是成本，多数不可见时减少不透明 Draw。阴影投射者另列，不使用主相机剔除。", "",
              "同一配置/种子/相机、源与程序摘要；开关顺序按轮次交替。Present interval=1，逐帧 occluded 和 raw delta 保留；wall FPS 不代表纯渲染吞吐。普通桌面负载未隔离，温度/功耗仅起止观测，没有改变电源或频率。"]
    (args.input / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    digests = [{"path": str(path.relative_to(args.input)).replace("\\", "/"), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
               for path in sorted(args.input.rglob("*")) if path.is_file() and path.name != "artifact-digests.json"]
    (args.input / "artifact-digests.json").write_text(json.dumps(digests, indent=2) + "\n", encoding="utf-8")
    print(f"Validated culling comparison: {len(runs)} runs; formal={args.formal}; both full-image pairs identical.")


if __name__ == "__main__":
    main()
