"""按原始摘要归档作品包证据与冻结源码；不复制本地 AGENTS 或整个检查点。"""
import argparse
import hashlib
import json
import shutil
from pathlib import Path


def load(path):
    return json.loads(path.read_text(encoding="utf-8-sig"))


def checked(root, relative):
    path = Path(relative)
    if path.is_absolute() or ".." in path.parts or path.name.lower() == "agents.md":
        raise ValueError("Invalid delivery path: " + relative)
    target = (root / path).resolve()
    target.relative_to(root.resolve())
    return target


def copy_inputs(manifest, origins, destination, preserve_primary=False):
    source = load(manifest)
    combined = "".join(row["path"] + "\0" + row["sha256"] + "\n" for row in source["files"])
    assert hashlib.sha256(combined.encode("utf-8")).hexdigest() == source["source_sha256"]
    for row in source["files"]:
        candidates = [checked(origin, row["path"]) for origin in origins]
        original = next((path for path in candidates if path.is_file()), None)
        assert original and hashlib.sha256(original.read_bytes()).hexdigest() == row["sha256"], row["path"]
        # 历史分析器按缺失的两项确定补充清单；保持原检查点/补充归档分界。
        if preserve_primary and original != candidates[0]:
            continue
        target = checked(destination, row["path"])
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(original, target)
    return {"source_sha256": source["source_sha256"], "files": len(source["files"])}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    root, package, base = args.root.resolve(), args.package.resolve(), args.evidence.resolve()
    package.relative_to(root)
    relative = base.relative_to(root)
    target_base = package / relative
    target_base.mkdir(parents=True)
    proofs = {}
    for task in ("t30", "t31", "t32", "t33", "t34", "t37", "t39-presentation"):
        data = base / (task + "-formal-01")
        for row in load(data / "artifact-digests.json"):
            path = checked(data, row["path"])
            assert hashlib.sha256(path.read_bytes()).hexdigest() == row["sha256"]
        shutil.copytree(data, target_base / data.name)
        checkpoint = "checkpoint-t39-measurement" if task == "t39-presentation" else "checkpoint-" + task
        proofs[task] = copy_inputs(data / "source-inputs.json",
            [base / checkpoint / "files", base / "t39-source-supplement-01/files"], target_base / checkpoint / "files", preserve_primary=True)
    shutil.copytree(base / "t39-source-supplement-01", target_base / "t39-source-supplement-01")
    for name in ("t32-pix", "t34-pix"):
        target = target_base / name
        target.mkdir()
        for original in (base / name).iterdir():
            if original.is_file():
                shutil.copy2(original, target / original.name)
    reports = package / "Docs/Reports/T39"
    reports.parent.mkdir(parents=True)
    shutil.copytree(root / "Docs/Reports/T39", reports)
    archive = package / "SourceArchive"
    proofs["candidate"] = copy_inputs(package / "source-inputs.json", [root], archive)
    for name in ("README.md", "01_开发环境搭建.md", "02_系统详细设计.md", "03_开发执行计划.md"):
        original = root / name
        if original.is_file():
            shutil.copy2(original, archive / name)
    shutil.copytree(root / "Docs", archive / "Docs")
    (package / "evidence-index.json").write_text(json.dumps({"schema": 1, "base": relative.as_posix(),
        "source_proofs": proofs, "historical_runs": 57, "presentation_runs": 6,
        "local_agents_included": False}, indent=2) + "\n", encoding="utf-8")
    print("Evidence and exact source archives copied; local AGENTS excluded.")


if __name__ == "__main__":
    main()
