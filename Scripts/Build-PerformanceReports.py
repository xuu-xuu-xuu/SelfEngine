"""从历史冻结分析器重新核算原始数据，生成两份中文性能报告与图表输入。"""
import argparse
import hashlib
import importlib.util
import json
import math
import shutil
import statistics
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HISTORY = ("t30", "t31", "t32", "t33", "t34", "t37")


def read_json(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def module_at(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def same(actual, expected, label):
    if isinstance(expected, dict):
        assert isinstance(actual, dict) and actual.keys() == expected.keys(), (label, "keys")
        for key in expected:
            same(actual[key], expected[key], label + "." + key)
    elif isinstance(expected, list):
        assert isinstance(actual, list) and len(actual) == len(expected), (label, "length")
        for index, value in enumerate(expected):
            same(actual[index], value, label + "." + str(index))
    elif isinstance(expected, (int, float)) and not isinstance(expected, bool):
        assert math.isclose(actual, expected, rel_tol=1e-11, abs_tol=1e-10), (label, actual, expected)
    else:
        assert actual == expected, (label, actual, expected)


def checked_path(root, relative):
    path = Path(relative)
    assert not path.is_absolute() and ".." not in path.parts
    return root / path


def source_closure(base):
    """旧过滤器纳入的两项 .cache 只在哈希匹配时补归档，不改任何旧检查点。"""
    supplement = base / "t39-source-supplement-01"
    copied = {}
    proofs = {}
    for task in HISTORY:
        source = read_json(base / (task + "-formal-01") / "source-inputs.json")
        aggregate = "".join(row["path"] + "\0" + row["sha256"] + "\n" for row in source["files"])
        assert hashlib.sha256(aggregate.encode("utf-8")).hexdigest() == source["source_sha256"]
        snapshot = base / ("checkpoint-" + task) / "files"
        for row in source["files"]:
            archived = checked_path(snapshot, row["path"])
            if not archived.is_file():
                archived = checked_path(supplement / "files", row["path"])
                if not archived.is_file():
                    candidate = checked_path(ROOT, row["path"])
                    assert candidate.is_file() and digest(candidate) == row["sha256"], (task, "missing exact source", row["path"])
                    archived.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(candidate, archived)
                copied[row["path"]] = row["sha256"]
            assert digest(archived) == row["sha256"], (task, "source mismatch", row["path"])
        proofs[task] = {"source_sha256": source["source_sha256"], "verified_files": len(source["files"]), "checkpoint": "checkpoint-" + task}
    manifest = {"schema": 1, "reason": "historical source identity included ignored derived template cache; copied bytes match every historical recorded digest",
                "files": [{"path": name, "sha256": value} for name, value in sorted(copied.items())]}
    supplement.mkdir(parents=True, exist_ok=True)
    target = supplement / "manifest.json"
    if target.exists():
        same(read_json(target), manifest, "source supplement")
    else:
        target.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    return proofs, supplement


def historical(base):
    proofs, supplement = source_closure(base)
    results = {}
    for task in HISTORY:
        directory = base / (task + "-formal-01")
        expected = read_json(directory / "summary.json")
        assert expected["formal_protocol_passed"]
        for row in read_json(directory / "artifact-digests.json"):
            assert digest(checked_path(directory, row["path"])) == row["sha256"], (task, "artifact mismatch", row["path"])
        environment = expected["environment"]
        assert proofs[task]["source_sha256"] == environment["source_sha256"]
        name = "Analyze-Loading.py" if task == "t37" else "Analyze-Resources.py" if task == "t34" else "Analyze-Benchmarks.py"
        frozen = module_at("frozen_" + task, base / ("checkpoint-" + task) / "files/Scripts" / name)
        actual = []
        for run in expected["runs"]:
            item = frozen.validate(directory / run["run"], environment, True)
            same(item, run, task + "." + run["run"])
            actual.append(item)
        results[task] = {"environment": environment, "runs": actual, "source_proof": proofs[task], "directory": directory}
    return results, supplement


def group(runs, predicate):
    result = [row for row in runs if predicate(row)]
    assert len(result) == 3
    return result


def statistic(values):
    return {"mean": statistics.mean(values), "stddev": statistics.stdev(values), "values": values}


def scope(runs, key, quantile="median"):
    return statistic([run["metrics"][key][quantile] for run in runs])


def value(runs, key):
    return statistic([run[key] for run in runs])


def number(stat):
    return f"{stat['mean']:.6f} ± {stat['stddev']:.6f}"


def change(before, after):
    return f"{(after['mean'] / before['mean'] - 1) * 100:+.2f}%"


def relative(path, output):
    import os
    return Path(os.path.relpath(path, output)).as_posix()


def chart(name, title, unit, categories, series):
    return {"file": name, "title": title, "unit": unit, "categories": categories,
            "caption": "三次独立进程；柱为每轮统计量的均值，标签 ± 为三轮标准差。原始值见图表 JSON。", "series": series}


def series(name, color, stats):
    return {"name": name, "color": color, "points": stats}


def overview(history, output, supplement):
    lines = ["所有历史集合由其检查点中的原版分析器重读 CSV，逐运行结果与原 summary.json 递归比对；逐文件校验 artifact-digests.json。没有用新版过滤器改写旧数据。", "",
             "| 集合 | 运行数 | 源码输入文件 | 源码 SHA-256 | 程序 SHA-256 | 原始数据 |", "|---|---:|---:|---|---|---|"]
    for task, data in history.items():
        e = data["environment"]
        link = relative(data["directory"] / "summary.json", output)
        lines.append(f"| {task.upper()} | {len(data['runs'])} | {data['source_proof']['verified_files']} | `{e['source_sha256']}` | `{e['executable_sha256']}` | [summary]({link}) |")
    lines += ["", f"源码原字节在各 checkpoint-*/files。旧摘要还包含两份被 Git 忽略的派生缓存，补充归档 [manifest]({relative(supplement / 'manifest.json', output)}) 的字节与全部旧摘要相同；原检查点保持原状。当前 SourceIdentity 排除 .cache，后续运行不会因生成缓存改变源码身份。",
              "", "相同 HEAD 不代表相同未提交源码。各组只在同次冻结版本内作前后对照，不跨版本直接比较绝对耗时；重建程序应重新记录其摘要，不承诺重新链接后 PE 文件逐字节相同。正式 Git 提交待用户审查保存，未替用户提交。"]
    return lines


def drawing(history, output, presentation, presentation_directory=None):
    culling, sorting, instances = (history[key]["runs"] for key in ("t31", "t32", "t33"))
    lines = ["# 大量物体绘制：可见性、排序与实例化", "",
             "1000 对象固定场景中，剔除在多数对象位于视锥外时减少实际绘制；实例化降低重复提交的 CPU 工作。排序虽显著减少绑定，当前实现的排序成本超过录制节省。混合实例化出现 GPU 成本，所有结果按各自冻结版本保留。", "",
             "## 输入与测量边界", "",
             "固定种子 424242、1280×720、5 秒预热 + 60 秒采样、每路三次独立进程，真实时间相机周期 60 秒。剔除使用算法 2 的全可见/90% 在外两种构造；排序、实例化使用各自固定混合/重复配置，输入布局摘要逐轮相同，不能把不同构造当同一工作负载。",
             "", "表格的毫秒值是三轮各自中位数的均值 ± 三轮标准差；相对变化由这些均值计算。P95/P99 在各轮内采用 nearest-rank ceil(p×N)，不是把三轮分位数混为总体分位数。CPU.CommandPrepare 包含排序/实例化等子区间，CPU.Frame 包含准备、记录和提交等待；子区间不可直接相加，CPU 与 GPU 不可相加。",
             "", "历史采样全部 occluded，帧外有 16 ms sleep，Present interval=1。历史数据可比较工作开销，不据其墙钟或 GPU 毫秒推导可见 FPS。实际绑定在 Native Set 调用处计数；CPU 准备结束后才进行 Native 录制。", "",
             "## 视锥剔除", "", "| 构造 | CPU 准备关闭/开启 ms | 变化 | GPU 关闭/开启 ms | 变化 | Draw 关闭→开启 |", "|---|---|---:|---|---:|---|"]
    for case, label in (("all_visible", "全可见"), ("mostly_outside", "90% 在外")):
        off = group(culling, lambda r: r["visibility_case"] == case and not r["culling"])
        on = group(culling, lambda r: r["visibility_case"] == case and r["culling"])
        prep0, prep1, gpu0, gpu1 = scope(off, "CPU.CommandPrepare"), scope(on, "CPU.CommandPrepare"), scope(off, "gpu_frame_ms"), scope(on, "gpu_frame_ms")
        lines.append(f"| {label} | {number(prep0)} / {number(prep1)} | {change(prep0, prep1)} | {number(gpu0)} / {number(gpu1)} | {change(gpu0, gpu1)} | {off[0]['draws']['median']}→{on[0]['draws']['median']} |")
    lines += ["", "全可见时边界缓存与逐项测试仍有成本。多数在外时保守 AABB 剔除使 1000 次 Draw 降为 100 次；模型数据仍正常加载，这不是资产流式可见性系统。相机外阴影投射列表单独保留，正确性由独立 GPU 图验证；奇异/非仿射等异常边界走保守保留，不以错误剔除换收益。", "",
              "## 排序与绑定", "", "| 路径 | CPU 准备 ms | Native 录制 ms | CPU Frame ms | 表 / VB / IB 绑定 |", "|---|---|---|---|---|"]
    sorted_groups = {}
    for key, label in (("raw", "原序"), ("cache", "仅缓存"), ("sorted", "排序+缓存")):
        runs = group(sorting, lambda r: r["run"].startswith(key + "-"))
        sorted_groups[key] = runs
        bindings = runs[0]["opaque_bindings"]
        calls = " / ".join(str(bindings[name]["median"]) for name in ("opaque_table_binds", "opaque_vertex_binds", "opaque_index_binds"))
        lines.append(f"| {label} | {number(scope(runs,'CPU.CommandPrepare'))} | {number(scope(runs,'CPU.CommandRecord'))} | {number(scope(runs,'CPU.Frame'))} | {calls} |")
    sort_cost = scope(sorted_groups["sorted"], "CPU.OpaqueSort")
    lines += ["", f"排序子区间为 {number(sort_cost)} ms；Native 录制下降，但总 CPU Frame 变化 {change(scope(sorted_groups['raw'],'CPU.Frame'),scope(sorted_groups['sorted'],'CPU.Frame'))}。实际 PSO/Root 仍各 1，Draw/逐 Draw 常量仍各 1000；表降至 4、VB/IB 至 48 已由真实 PIX 导出复核。当前不把排序默认为开，不宣称仅绑定计数下降就等于总体加速。", "",
              "## 实例化", "", "| 场景 | CPU Frame 关闭/开启 ms | 变化 | GPU 关闭/开启 ms | 变化 | Draw 关闭→开启 |", "|---|---|---:|---|---:|---|"]
    off_groups, on_groups = [], []
    for scene, label in (("repeated", "重复"), ("mixed", "混合")):
        off = group(instances, lambda r: r["scene"] == scene and not r["instancing"])
        on = group(instances, lambda r: r["scene"] == scene and r["instancing"])
        off_groups.append(off);on_groups.append(on)
        cpu0, cpu1, gpu0, gpu1 = scope(off,"CPU.Frame"),scope(on,"CPU.Frame"),scope(off,"gpu_frame_ms"),scope(on,"gpu_frame_ms")
        lines.append(f"| {label} | {number(cpu0)} / {number(cpu1)} | {change(cpu0,cpu1)} | {number(gpu0)} / {number(gpu1)} | {change(gpu0,gpu1)} | {off[0]['draws']['median']}→{on[0]['draws']['median']} |")
    lines += ["", "![实例化 CPU](instancing-cpu.png)", "", "![实例化 GPU](instancing-gpu.png)", "",
              "Raw 实例每个 240 字节，1000 个实例每帧 240000 字节。重复/混合的逐 Draw CBV 从 512000 字节分别降至 512 / 24576 字节；两帧 UploadArena 的保留量单列，不把实例 payload 当全部显存。完整纹理/材质、镜像/剪切/法线与父子矩阵保持同图，真实 Gate 保护 Frame 上传内容。当前仅 PBR/Benchmark 支持此实例路径，Forward/Shadow/Environment 未提前计完成。", "",
              "重复几何在 CPU/GPU 都降低；混合场景 CPU 降低而 GPU 上升，可能涉及实例数据访问/着色路径等多个因素，当前证据只能确认代价，不能把未测的硬件原因当结论。", "",
              "## 普通场景呈现"]
    if presentation is None:
        lines += ["", "当前是历史证据预生成版本，可见/隐藏普通场景正式协议尚未完成，不检查 60 Hz 目标。"]
    else:
        lines += ["", "同一新版本、32 个混合物体、1280×720、全部优化关闭、同一固定相机函数；可见/隐藏交替，每路三次 5+60 秒。下表的 Hz 为相邻已记录样本数量 / 样本时间差，是窗口渲染提交节奏，不等于面板逐帧扫描输出。", "",
                  "| 路径 | 采样帧 | occluded | 平均提交 Hz（各轮） | CPU Frame 中位均值 ms | Submit/Present 中位均值 ms | GPU 中位均值 ms |", "|---|---:|---:|---|---:|---:|---:|"]
        for name, label in (("visible","可见"),("hidden","隐藏")):
            item=presentation["groups"][name];m=item["mean_run_medians_ms"]
            rates=[run['wall_submission_hz'] for run in presentation['runs'] if run['window_visible']==(name=='visible')]
            lines.append(f"| {label} | {item['sample_frames']} | {item['occluded_frames']} | {' / '.join(f'{v:.3f}' for v in rates)} | {m['CPU.Frame']:.6f} | {m['CPU.SubmitPresent']:.6f} | {m['gpu_frame_ms']:.6f} |")
        visible=presentation["groups"]["visible"]
        desktops=sorted({r['window_station']+'/'+r['thread_desktop'] for r in presentation['runs']})
        environment=presentation['environment']
        source_id=environment['source_sha256'];program_id=environment['executable_sha256']
        evidence_link=relative(presentation_directory/'summary.json',output);desktop_text=' / '.join(desktops)
        image_id=presentation['image_sha256']
        lines += ["", f"此集合源码 `{source_id}`，程序 `{program_id}`；[正式原始数据]({evidence_link})。",
                  "", f"执行桌面：{desktop_text}；同场景独立抓图完整 SHA-256 为 `{image_id}`。",
                  "每帧实际尺寸、可见/前台标记与 occlusion 都保留。工具默认隔离桌面的短协议全部 occluded，隔离外短测才确认可见路径；没有把默认桌面的历史状态追溯猜成事实。",
                  "", f"窗口实际可见且 Present 未报 occluded：{visible['all_presentations_verified']}；标称 60Hz 的平均提交目标检查：{visible['all_average_60hz_targets_met']}。预先采用 59Hz 下限容纳刷新偏差，逐轮原值与所有 >16.67ms 间隔保留；不承诺最小 FPS、每帧扫描显示或所有游戏负载。隐藏窗口即使返回非 occluded 也不算可见提交，帧外 sleep 同样影响墙钟节奏。"]
        m=visible['mean_run_medians_ms']
        lines += ["", "## 瓶颈判断与本步取舍", "",
                  f"此普通场景的 CPU 准备中位均值为 {m['CPU.CommandPrepare']:.6f} ms，Native 录制 {m['CPU.CommandRecord']:.6f} ms，GPU Frame {m['gpu_frame_ms']:.6f} ms；它们均低于 16.67ms 预算。",
                  f"CPU.Frame 中位均值 {m['CPU.Frame']:.6f} ms 与墙钟间隔 {m['wall_delta_ms']:.6f} ms 的边界不同；不能把全部差值分配给一个等待函数，也不能仅据 CPU.Frame 大于 GPU Frame 判定计算型 CPU-bound。",
                  "", "单变量对照给出更明确的改进方向：大量物体时 CPU 准备/逐 Draw 提交值得处理，绑定排序没有提供总体收益，混合实例化出现局部 GPU 成本。普通场景平均目标已达到，本步完成诊断与复测，不再凭 GPU 毫秒的倒数追加优化。"]
    lines += ["", "GPU 区间按队列 timestamp 差值 / 该队列 frequency 换算毫秒，未与 CPU 绝对时钟直接对齐；定义参考 [D3D12 Timing](https://learn.microsoft.com/en-us/windows/win32/direct3d12/timing)。",
              "Present 的同步参数影响呈现队列，Sleep 的实际恢复也取决于时钟与调度；定义参考 [Present](https://learn.microsoft.com/en-us/windows/win32/api/dxgi/nf-dxgi-idxgiswapchain-present)、[Sleep](https://learn.microsoft.com/en-us/windows/win32/api/synchapi/nf-synchapi-sleep)。本报告未推断每个面板扫描时刻。",
              "", "## 环境、正确性与复测", "",
              "RTX 3060 Laptop（专用显存约 6 GB）、i7-12700H、20 硬件线程，Windows build 22631.6199 / 23H2，驱动和电源起止见各 run/environment/sensors。普通桌面负载未隔离，不锁频、不改电源计划；温度/频率是端点观测，不是峰值或稳定频率保证。", "",
              "T31：12000 组独立包围盒 oracle/相机路线/相机外 caster；T32：四份 PIX 图与原生全图一致；T33：独立经典路径与实例路径逐图一致。PIX GPU 抓帧用于实际调用/资源检查，T18 ETW 记录有丢事件，未据其推导整机时序。原始抓帧与分析见对应任务包。", "",
              "最新源码从仓库根目录构建 Release，以下命令逐条执行并检查退出码；输出目录必须尚不存在。Presentation 需要用户实际桌面，工具隔离桌面只能验证其自身状态。`-Smoke -Repeats 1` 只检查协议。历史版本从下方检查点恢复，不将新版结果写入旧集合。", "",
              '```powershell','powershell -NoProfile -File Scripts/Build-Test.ps1 -Configuration Release','```','']
    for suite in ('Culling','Sorting','Instancing','Presentation'):
        lines += ['```powershell',f'powershell -NoProfile -File Scripts/Run-Benchmarks.ps1 -Suite {suite} -OutputDirectory out/benchmarks/reproduce-{suite.lower()}-01','```','']
    charts = [chart("instancing-cpu.png","实例化：CPU Frame（每轮中位数）","毫秒",["重复场景","混合场景"],[series("关闭","#4c78a8",[scope(r,"CPU.Frame") for r in off_groups]),series("开启","#e58c45",[scope(r,"CPU.Frame") for r in on_groups])]),
              chart("instancing-gpu.png","实例化：GPU Frame（每轮中位数）","毫秒",["重复场景","混合场景"],[series("关闭","#4c78a8",[scope(r,"gpu_frame_ms") for r in off_groups]),series("开启","#e58c45",[scope(r,"gpu_frame_ms") for r in on_groups])])]
    return lines, charts


def resources(history):
    lifecycle, loading = history["t34"]["runs"],history["t37"]["runs"]
    cases=[]
    lines=["# 资源管理与后台加载：成本、寿命与延迟", "",
           "固定资源循环中，批次上传/缓存减少真实创建与提交，但 placed 分配保留更多空闲容量。运行中后台加载降低长尾，却显著延后全部资源就绪；预算将上传工作分摊到更多帧，总工作量和就绪延迟分别记录。", "",
           "## 资源循环", "",
           "同一源版本与输入：24 张 257×193 图片、6 组立方体 VB/IB、16 次等价管线请求；4 次预热 + 24 次采样循环，每路三次。只切换 placed/committed、batch/individual、PSO cached/uncached 中的一项，实际 Draw 都为 24。加载到可用、退出到 Fence 物理退休分别计时，显式 drain/Trim 在采样区间外。", "",
           "下表为三轮各自中位数的均值 ± 标准差；内存为 API 分配/预留，UploadArena 单列，不等同于整机或驱动物理驻留显存。", "",
           "| 单变量路径 | 加载就绪 ms | 退休 ms | upload 提交 | PSO 创建 | loaded / drained reserved MiB |", "|---|---|---|---:|---:|---|"]
    for key,label in (("baseline","placed+batch+cache"),("committed","committed"),("individual","逐资源上传"),("uncached","禁用 PSO cache")):
        runs=group(lifecycle,lambda r:r["case"]==key);cases.append(runs)
        lines.append(f"| {label} | {number(scope(runs,'load_ready_ms'))} | {number(scope(runs,'retire_ms'))} | {runs[0]['upload_submissions']} | {runs[0]['pso_creations']} | {runs[0]['loaded_memory_reserved']/1048576:.4f} / {runs[0]['drained_memory_reserved']/1048576:.4f} |")
    lines += ["", "![资源循环](resource-load.png)", "",
              "placed loaded/drained reserved 保留 16 MiB，而 used 与 committed 相同；Trim 后只保留约 4 MiB 的常驻窗口 heap。此数据中 committed 的创建/退休成本更高，不能据此声称 placed 始终更省内存。每轮循环的稳定高水位、CPU descriptor 与物理资源退休都已核对。", "",
              "真实 PIX 引擎 Direct queue 的 ExecuteCommandLists 为 batch 2 / individual 37（含一次 Draw）；额外捕获/辅助队列操作不计为引擎上传。实际应用上传复制为 24 Texture + 12 Buffer，24 Draw；输出全图相同。实际阻塞 Fence 调用与显式全局 flush 分开，不把等待调用数量或 GPU Draw timestamp 当成上传 GPU 毫秒。", "",
              "## 运行中的加载", "",
              "16 张 1025×513 BMP，固定种子 424242、内容摘要相同。每次 480 帧，第 30 帧触发；同步/后台各三次独立进程，交替次序。后台实际上限 262144 字节 staging / 4 个复制任务每帧，行 pitch/placement 对齐计入预算；WIC、布局与图片键在 worker，GPU 资源创建/提交/回执在主线程。文件校验在采样前，OS 文件缓存已预热。", "",
              "总就绪与最差帧是每轮各一个值，表中为三轮均值 ± 标准差；CPU P95/P99 是三轮各自分位数的均值 ± 标准差，未将三轮合并成总体分位数。", "",
              "| 路径 | 总就绪 ms（均值 ± sd） | CPU P95 ms | CPU P99 ms | 最差 CPU 帧 ms（均值 ± sd） | >16.67ms 帧数/轮 |", "|---|---|---|---|---|---|"]
    loading_groups=[]
    for mode,label in (("sync","同步"),("async","后台")):
        runs=group(loading,lambda r:r["strategy"]==mode);loading_groups.append(runs)
        lines.append(f"| {label} | {number(value(runs,'total_ready_ms'))} | {number(scope(runs,'CPU.Frame','p95'))} | {number(scope(runs,'CPU.Frame','p99'))} | {number(value(runs,'cpu_worst_ms'))} | {' / '.join(str(r['cpu_over_16_67_ms']) for r in runs)} |")
    sync,async_runs=loading_groups
    ratio=value(async_runs,"total_ready_ms")["mean"]/value(sync,"total_ready_ms")["mean"]
    p95=change(scope(sync,"CPU.Frame","p95"),scope(async_runs,"CPU.Frame","p95"))
    worst=[r["cpu_worst_ms"] for r in async_runs]
    lines += ["", "![加载长尾](loading-worst.png)", "", "![全部就绪时间](loading-ready.png)", "",
              f"后台总就绪约为同步 {ratio:.2f} 倍，CPU P95 变化 {p95}。后台各轮最差帧为 {' / '.join(f'{v:.4f}' for v in worst)} ms，最差值的轮间 CV 为 {statistics.stdev(worst)/statistics.mean(worst):.2%}；没有删除 10.49ms 尖峰，也不能承诺未来不会出现长帧。最差帧数值来自 CPU.Frame，不等于 GPU Frame 或墙钟间隔。", "",
              "逻辑 payload 共 33652800 字节，实际 padded staging 35725824 字节（约 +6.16%）；该集合理论至少需 ceil(staging/262144)=137 个满预算上传帧，实际有上传的帧也是 137，累计 152 个复制任务。第 170 帧全部就绪，包含 CPU 准备/发布/排队与 Fence 可用确认；不能把这些阶段全部归为 GPU 复制耗时。", "",
              "两路峰值 DEFAULT used/reserved 都为 40304640 / 75497472 字节；示例 UploadArena retained 同步 4718592、后台 393216，后者降低 staging 高水位但未减少最终纹理存储。后台实际阻塞 Fence 调用仍有 0/0/2 次，不能称普通路径完全无等待。关闭后 retiring/in-flight 为零且资源/CPU SRV/used 回常驻基线。", "",
              "## 就绪契约与正确性", "",
              "CPU 任务共享候选但订阅可独立取消；最后订阅释放后取消 worker/上传，不将已经记录到 GPU 的资源提前释放。CPU payload 子对象 alias 继续占预算；新场景同 ID 与旧 operation/revision/attempt 回执区分，真实 Fence 完成后才发布 Ready。占位图不冒充实际 Ready，非法最小预算明确失败，加载中退出回收资源。", "",
              "独立 staged/whole 上传覆盖奇数宽纹理、全部 texel、模型/PBR/变换；真实 Queue Gate 验证在途寿命。最终 640×480 同步/后台完整图相同，PPM SHA-256 为 019e05bbb7f3c3d9497f0ac53a399dc2f91d3c4eba28cab6d554fdf3ce9c4edf。", "",
              "## 适用范围与复测", "",
              "CPU.Frame 窗口是第 30 帧起共 451 个样本/轮，GPU timestamp 仅 Draw，未覆盖上传批次。所有测量帧 occluded，16ms 帧外节流与 raw delta 单列；这些数据不证明可见 FPS。CPU/GPU 区间可重叠，解码/提交/等待也是不同阶段，不能相加拼出未经测量的总时序。", "",
              "运行环境为 RTX3060 Laptop / i7-12700H、Windows build 22631.6199 /23H2，驱动与电源见原始元数据。桌面未隔离、未锁频；温度/功耗只记录起止，不推断峰值。传统图形抓帧检查调用与像素，已知丢事件 ETW 不用于完整系统瓶颈归因。", "",
              "从仓库根目录构建 Release，再逐条执行以下命令并检查退出码；默认要求各路三轮正式数据，Smoke 只验协议。输出目录必须尚不存在，源码/程序/输入摘要保留；截图进程独立于采样。", "",
              '```powershell','powershell -NoProfile -File Scripts/Run-ResourceBenchmarks.ps1 -OutputDirectory out/benchmarks/reproduce-resources-01','```','',
              '```powershell','powershell -NoProfile -File Scripts/Run-LoadingBenchmarks.ps1 -OutputDirectory out/benchmarks/reproduce-loading-01','```','']
    charts=[chart("resource-load.png","资源循环：加载到可用","毫秒",["默认","committed","逐资源上传","无 PSO 缓存"],[series("加载就绪","#4c78a8",[scope(r,"load_ready_ms") for r in cases])]),
            chart("loading-worst.png","运行中加载：最差 CPU Frame","毫秒",["同步","后台"],[series("每轮最差帧","#4c78a8",[value(r,"cpu_worst_ms") for r in loading_groups])]),
            chart("loading-ready.png","运行中加载：全部资源就绪时间","毫秒",["同步","后台"],[series("全部就绪","#e58c45",[value(r,"total_ready_ms") for r in loading_groups])])]
    return lines,charts


def main():
    parser=argparse.ArgumentParser()
    parser.add_argument("--evidence",type=Path,required=True)
    parser.add_argument("--output",type=Path,required=True)
    parser.add_argument("--presentation",type=Path)
    args=parser.parse_args()
    base=args.evidence.resolve();output=args.output.resolve();output.mkdir(parents=True,exist_ok=True)
    history,supplement=historical(base)
    presentation=None
    if args.presentation:
        module=module_at("presentation_analysis",Path(__file__).with_name("Analyze-Presentation.py"))
        presentation=module.validate(args.presentation.resolve(),True)
        same(presentation,read_json(args.presentation/"summary.json"),"presentation")
        for row in read_json(args.presentation/"artifact-digests.json"):
            assert digest(checked_path(args.presentation,row['path']))==row['sha256'],('presentation artifact',row['path'])
        inputs=read_json(args.presentation/"source-inputs.json")
        combined=''.join(row['path']+'\0'+row['sha256']+'\n' for row in inputs['files'])
        assert hashlib.sha256(combined.encode('utf-8')).hexdigest()==presentation['environment']['source_sha256']
        for row in inputs['files']:
            assert digest(checked_path(base/'checkpoint-t39-measurement/files',row['path']))==row['sha256'],('presentation source',row['path'])
    drawing_lines,drawing_charts=drawing(history,output,presentation,args.presentation.resolve() if args.presentation else None)
    resource_lines,resource_charts=resources(history)
    audit=overview(history,output,supplement)
    for name,lines in (("Drawing.md",drawing_lines),("Resources.md",resource_lines)):
        (output/name).write_text("\n".join(lines+["## 版本与原始数据","",*audit])+"\n",encoding="utf-8")
    charts={"schema":1,"charts":drawing_charts+resource_charts,"source_proofs":{task:data["source_proof"] for task,data in history.items()}}
    (output/"chart-data.json").write_text(json.dumps(charts,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")
    verification={"schema":1,"historical_runs":sum(len(data["runs"]) for data in history.values()),"historical_sets":len(history),"all_artifact_digests_verified":True,
                  "all_frozen_analyzer_results_equal":True,"source_proofs":charts["source_proofs"],"presentation_formal":presentation is not None,
                  "visible_presentation_verified":presentation["groups"]["visible"]["all_presentations_verified"] if presentation else False,
                  "presentation_source_sha256":presentation['environment']['source_sha256'] if presentation else None,
                  "generator_sha256":digest(Path(__file__))}
    (output/"verification.json").write_text(json.dumps(verification,indent=2,ensure_ascii=False)+"\n",encoding="utf-8")
    print(f"Performance reports generated from {verification['historical_runs']} revalidated historical runs; presentation formal={presentation is not None}.")


if __name__=="__main__":
    main()
