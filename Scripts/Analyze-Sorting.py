"""分别检查原始绑定、仅缓存和排序加缓存，保留排序成本与真实 API 调用数。"""
from pathlib import Path
import argparse
import hashlib
import importlib.util
import json
import statistics

spec = importlib.util.spec_from_file_location("benchmark_analysis", Path(__file__).with_name("Analyze-Benchmarks.py"))
baseline = importlib.util.module_from_spec(spec)
spec.loader.exec_module(baseline)


def expected_groups(count, seed):
    state = seed
    selectors = []
    for _ in range(count):
        for _ in range(6):
            state = (state ^ (state << 13)) & 0xffffffff
            state ^= state >> 17
            state = (state ^ (state << 5)) & 0xffffffff
        selectors.append(((state >> 1) % 24, bool(state & 1)))
    textures = [material % 4 for material, _ in selectors]
    meshes = [sphere for _, sphere in selectors]
    changes = lambda values: 1 + sum(a != b for a, b in zip(values, values[1:]))
    return {"table_changes": changes(textures), "mesh_changes": changes(meshes),
            "tables": len(set(textures)), "groups": len(set(selectors))}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--input", type=Path, required=True)
    parser.add_argument("--formal", action="store_true")
    args = parser.parse_args()
    environment = json.loads((args.input / "environment.json").read_text(encoding="utf-8"))
    assert environment["suite"] == "Sorting"
    runs = [baseline.validate(path.parent, environment, args.formal) for path in sorted(args.input.glob("*/run.json"))]
    assert len(runs) == environment["repetitions"] * 3
    captures = args.input / "captures"
    images, comparisons = [], {}
    for label, sorting, caching in (("raw", False, False), ("cache", False, True), ("sorted", True, True)):
        shot = json.loads((captures / label / "run.json").read_text(encoding="utf-8"))
        assert shot["success"] and shot["mode"] == "capture" and not shot["measurement_complete"]
        assert shot["sorting"] == sorting and shot["binding_cache"] == caching and not shot["culling"]
        assert shot["source_sha256"] == environment["source_sha256"] and shot["executable_sha256"] == environment["executable_sha256"]
        images.append((captures / (label + ".ppm")).read_bytes())
        group = [run for run in runs if run["sorting"] == sorting and run["binding_cache"] == caching]
        assert len(group) == environment["repetitions"] and (not args.formal or len(group) >= 3)
        for run in group:
            assert run["scene"] == "mixed" and run["visibility_case"] == "all_visible" and not run["culling"]
            assert run["draws"]["median"] == run["object_count"]
            metadata = json.loads((args.input / run["run"] / "run.json").read_text(encoding="utf-8"))
            expected = expected_groups(run["object_count"], metadata["seed"])
            binds = run["opaque_bindings"]
            for key in binds:
                assert binds[key]["p95"] == binds[key]["median"] == binds[key]["p99"], "static bindings changed during route"
            tables, vertex = binds["opaque_table_binds"]["median"], binds["opaque_vertex_binds"]["median"]
            assert binds["opaque_index_binds"]["median"] == vertex
            if caching and not sorting:
                assert tables == expected["table_changes"] and vertex == expected["mesh_changes"], "API changes disagree with independent input sequence"
            if sorting:
                assert tables == expected["tables"] and vertex <= expected["groups"]
                if expected["groups"] == 48:
                    assert vertex == 48
        gpu = [run["metrics"]["gpu_frame_ms"]["median"] for run in group]
        comparisons[label] = {"runs": [run["run"] for run in group], "draws": group[0]["draws"]["median"],
            "gpu_ms": statistics.mean(gpu), "gpu_median_cv": statistics.stdev(gpu) / statistics.mean(gpu) if len(gpu) > 1 else None,
            "prepare_ms": statistics.mean(run["metrics"]["CPU.CommandPrepare"]["median"] for run in group),
            "record_ms": statistics.mean(run["metrics"]["CPU.CommandRecord"]["median"] for run in group),
            "frame_ms": statistics.mean(run["metrics"]["CPU.Frame"]["median"] for run in group),
            "sort_ms": statistics.mean(run["metrics"].get("CPU.OpaqueSort", {"median": 0})["median"] for run in group),
            "bindings": {key: values["median"] for key, values in group[0]["opaque_bindings"].items()}}
    assert images[0] == images[1] == images[2], "sorting/cache changed complete GPU image"
    assert len({run["layout_sha256"] for run in runs}) == 1
    for item in comparisons.values():
        item["prepare_change_percent"] = (item["prepare_ms"] / comparisons["raw"]["prepare_ms"] - 1) * 100
        item["record_change_percent"] = (item["record_ms"] / comparisons["raw"]["record_ms"] - 1) * 100
        item["frame_change_percent"] = (item["frame_ms"] / comparisons["raw"]["frame_ms"] - 1) * 100
    result = {"schema": 1, "formal_protocol_passed": args.formal, "environment": environment, "runs": runs,
              "comparisons": comparisons, "image_sha256": hashlib.sha256(images[0]).hexdigest()}
    (args.input / "summary.json").write_text(json.dumps(result, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    lines = ["# 不透明排序与绑定缓存对照", "", f"正式协议通过：{args.formal}；源 `{environment['source_sha256']}`。", "",
             "各数值为每轮中位数的算术均值；全部 P95/P99、原始 CSV 和三轮 GPU CV 见 summary.json。", "",
             "| 路径 | Draw | 纹理表/VB/IB Set | CPU 准备 ms | 其中排序 ms | CPU 录制 ms | CPU Frame ms | GPU ms |", "|---|---:|---|---:|---:|---:|---:|---:|"]
    for label, item in comparisons.items():
        binds = item["bindings"]
        lines.append(f"| {label} | {item['draws']:.0f} | {binds['opaque_table_binds']:.0f}/{binds['opaque_vertex_binds']:.0f}/{binds['opaque_index_binds']:.0f} | {item['prepare_ms']:.6f} | {item['sort_ms']:.6f} | {item['record_ms']:.6f} | {item['frame_ms']:.6f} | {item['gpu_ms']:.6f} |")
    lines += ["", "PSO 与 Root Signature 原来已每个 Opaque Pass 绑定一次，三条路径均为 1；每 Draw CBV 保留，不能把这些计数写成排序收益。",
              "CPU.OpaqueSort 已包含在 CPU.CommandPrepare；实际 Set 调用位于随后 Submit 中的 CPU.CommandRecord，所以两列必须分别看，不能仅凭准备时间判断绑定缓存收益。CPU.Frame 包含这些区间及更新/提取/Present，不能与子区间相加。缓存仅覆盖同一个 Pass，不沿用上帧或 Shadow/Sky/UI 状态。", "",
              f"三条路径完整采图 SHA-256 `{result['image_sha256']}` 相同。只处理不透明队列，同键稳定；精确共面且不同材质的重叠面可能依赖原先深度顺序，未来半透明不能直接套用。", "",
              "Present interval=1；occluded/raw delta 保留，wall FPS 不作为纯渲染吞吐。普通桌面后台未隔离，温度/功耗仅起止观测，没有修改电源或频率。"]
    (args.input / "report.md").write_text("\n".join(lines) + "\n", encoding="utf-8")
    digests = [{"path": str(path.relative_to(args.input)).replace("\\", "/"), "sha256": hashlib.sha256(path.read_bytes()).hexdigest()}
               for path in sorted(args.input.rglob("*")) if path.is_file() and path.name != "artifact-digests.json"]
    (args.input / "artifact-digests.json").write_text(json.dumps(digests, indent=2) + "\n", encoding="utf-8")
    print(f"Validated sorting comparison: {len(runs)} runs; formal={args.formal}; three full images identical.")


if __name__ == "__main__":
    main()
