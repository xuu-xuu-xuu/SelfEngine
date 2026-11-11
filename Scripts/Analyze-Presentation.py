"""普通场景可见/隐藏呈现口径；仅全部可见且无 occlusion 的样本可检查可见提交节奏。"""
import argparse
import hashlib
import importlib.util
import json
import statistics
from pathlib import Path


def load_benchmark_analyzer():
    spec = importlib.util.spec_from_file_location("engine_benchmark_analysis", Path(__file__).with_name("Analyze-Benchmarks.py"))
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def validate(root, formal):
    base = load_benchmark_analyzer()
    environment = json.loads((root / "environment.json").read_text(encoding="utf-8-sig"))
    assert environment["suite"] == "Presentation"
    if formal:
        assert not environment["smoke"] and environment["repetitions"] >= 3
    runs = []
    for path in sorted(root.glob("*/run.json")):
        directory = path.parent
        run = json.loads(path.read_text(encoding="utf-8"))
        item = base.validate(directory, environment, formal)
        assert run["surface_size_guard"] and run["size"] == [1280, 720]
        assert run["scene"] == "mixed" and run["object_count"] == 32 and run["present_sync_interval"] == 1
        assert not any(run[key] for key in ("culling", "sorting", "binding_cache", "instancing"))
        assert run["outside_frame_sleep_ms"] == {"occluded_or_nondrawable": 16, "drawable": 1}
        frames = base.read_csv(directory / "frames.csv")
        sample = [row for row in frames if row["phase"] == "sample"]
        assert all(int(row["surface_width"]) == 1280 and int(row["surface_height"]) == 720 for row in frames)
        visible = run["window_visible"]
        assert directory.name.startswith("visible-" if visible else "hidden-")
        assert all(int(row["window_visible"]) == int(visible) for row in sample)
        seconds = float(sample[-1]["sample_time_s"]) - float(sample[0]["sample_time_s"])
        assert seconds > 0
        cadence = (len(sample) - 1) / seconds
        present_verified = visible and item["sample_occluded_frames"] == 0
        item.update({"window_visible": visible, "window_station": run["window_station"], "thread_desktop": run["thread_desktop"],
                     "window_foreground_fraction": statistics.mean(int(row["window_foreground"]) for row in sample),
                     "sample_elapsed_s": seconds, "wall_submission_hz": cadence, "visible_presentation_verified": present_verified,
                     "visible_average_60hz_target_met": bool(present_verified and cadence >= 59.0),
                     "wall_intervals_over_16_67_ms": sum(float(row["raw_delta_ms"]) > 16.67 for row in sample)})
        runs.append(item)
    assert len(runs) == environment["repetitions"] * 2
    assert len({run["layout_sha256"] for run in runs}) == 1
    assert len({(run["window_station"], run["thread_desktop"]) for run in runs}) == 1
    images = {}
    for label in ("visible", "hidden"):
        capture = json.loads((root / "captures" / label / "run.json").read_text(encoding="utf-8"))
        assert capture["success"] and capture["mode"] == "capture" and not capture["measurement_complete"]
        assert capture["executable_sha256"] == environment["executable_sha256"]
        assert capture["layout_sha256"] == runs[0]["layout_sha256"] and capture["source_sha256"] == environment["source_sha256"]
        assert capture["window_visible"] == (label == "visible")
        images[label] = hashlib.sha256((root / "captures" / (label + ".ppm")).read_bytes()).hexdigest()
    assert images["visible"] == images["hidden"], "window modes changed the same captured scene"
    groups = {}
    for label, visible in (("visible", True), ("hidden", False)):
        group = [run for run in runs if run["window_visible"] == visible]
        assert len(group) == environment["repetitions"]
        frequencies = [run["wall_submission_hz"] for run in group]
        groups[label] = {"submission_hz": base.metric(frequencies),
                         "sample_frames": sum(run["sample_frames"] for run in group),
                         "occluded_frames": sum(run["sample_occluded_frames"] for run in group),
                         "all_presentations_verified": all(run["visible_presentation_verified"] for run in group),
                         "all_average_60hz_targets_met": all(run["visible_average_60hz_target_met"] for run in group),
                         "mean_run_medians_ms": {key: statistics.mean(run["metrics"][key]["median"] for run in group)
                                                 for key in ("CPU.Frame", "CPU.CommandPrepare", "CPU.CommandRecord", "CPU.SubmitPresent", "gpu_frame_ms", "wall_delta_ms")}}
    return {"schema": 1, "formal_protocol_passed": formal, "environment": environment, "runs": runs, "groups": groups, "image_sha256": images["visible"],
            "cadence_definition": "(sample_frames-1)/(last_sample_time-first_sample_time); 59Hz lower threshold allows 60Hz nominal refresh drift, not a minimum-FPS guarantee"}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--formal", action="store_true")
    args = parser.parse_args()
    result = validate(args.input, args.formal)
    (args.input / "summary.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    lines = ["# 普通场景呈现协议", "", f"正式集合完整：{args.formal}。32 个混合物体，1280×720，Present interval=1，交替可见/隐藏。", "",
             "帧间隔包含呈现等待和帧外 sleep；CPU.Frame 与 GPU 时间及各 CPU 子区间分别报告。可见但 occluded 时不宣称可见提交节奏达标。", "",
             "可见提交验证同时要求实际窗口可见和 Present 未报 occluded；隐藏窗口即使返回非 occluded 也不算。该检查不等于每帧被物理面板扫描显示。", "",
             "60Hz 平均目标采用 59Hz 下限容纳标称刷新偏差；这是平均提交节奏检查，不是最小 FPS 或无长帧承诺。", "",
             "| 轮次 | 样本 | occluded | 提交 Hz | CPU Frame 中位 ms | Submit/Present 中位 ms | GPU 中位 ms | 可见呈现证实 |", "|---|---:|---:|---:|---:|---:|---:|---|"]
    for run in result["runs"]:
        m = run["metrics"]
        lines.append(f"| {run['run']} | {run['sample_frames']} | {run['sample_occluded_frames']} | {run['wall_submission_hz']:.3f} | {m['CPU.Frame']['median']:.6f} | {m['CPU.SubmitPresent']['median']:.6f} | {m['gpu_frame_ms']['median']:.6f} | {run['visible_presentation_verified']} |")
    lines += ["", "起止温度/功耗为端点观测；运行期间未锁频、未改变电源方案。窗口前台比例保留，不推断用户实际观看或整机隔离。统计与全部原始数据见 summary.json 及每轮 CSV。"]
    (args.input / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    files = [{"path": path.relative_to(args.input).as_posix(), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
             for path in sorted(args.input.rglob("*")) if path.is_file() and path.name != "artifact-digests.json"]
    (args.input / "artifact-digests.json").write_text(json.dumps(files, indent=2) + "\n", encoding="utf-8")
    print(f"Presentation protocol validated: {len(result['runs'])} runs; formal={args.formal}; visible verified={result['groups']['visible']['all_presentations_verified']}.")


if __name__ == "__main__":
    main()
