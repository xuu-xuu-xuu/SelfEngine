"""检查本项目原生发布产物的 x64 PE 导入闭包，不加载或执行 DLL。"""
import argparse
import json
import struct
from pathlib import Path


def imports(path):
    data = path.read_bytes()
    assert data[:2] == b"MZ", path
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    assert data[pe:pe + 4] == b"PE\0\0", path
    machine, count, _, _, _, optional_size, _ = struct.unpack_from("<HHIIIHH", data, pe + 4)
    optional = pe + 24
    assert machine == 0x8664 and struct.unpack_from("<H", data, optional)[0] == 0x20B, (path, "expected x64")
    sections = []
    for index in range(count):
        offset = optional + optional_size + index * 40
        virtual_size, rva, raw_size, raw = struct.unpack_from("<IIII", data, offset + 8)
        sections.append((rva, max(virtual_size, raw_size), raw))

    def file_offset(rva):
        for start, size, raw in sections:
            if start <= rva < start + size:
                result = raw + rva - start
                assert result < len(data)
                return result
        raise ValueError("PE RVA outside sections")

    import_rva, _ = struct.unpack_from("<II", data, optional + 112 + 8)
    result = []
    if import_rva:
        cursor = file_offset(import_rva)
        while any(data[cursor:cursor + 20]):
            name = file_offset(struct.unpack_from("<I", data, cursor + 12)[0])
            result.append(data[name:data.index(b"\0", name)].decode("ascii"))
            cursor += 20
    return result


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--package", type=Path, required=True)
    parser.add_argument("--system", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    programs = list(args.package.glob("*.exe")) + list(args.package.glob("*.dll"))
    bundled = {path.name.lower() for path in programs}
    results = []
    for program in programs:
        rows = []
        for name in imports(program):
            lower = name.lower()
            local = lower in bundled
            if lower.startswith(("msvcp", "vcruntime", "concrt")):
                assert local, (program, "MSVC must be app-local", name)
            assert local or lower.startswith(("api-ms-", "ext-ms-")) or (args.system / name).is_file(), (program, name)
            rows.append({"name": name, "resolution": "app-local" if local else "Windows system"})
        results.append({"program": program.name, "machine": "AMD64", "imports": rows})
    assert (args.system / "d3dcompiler_47.dll").is_file(), "Windows FXC runtime unavailable"
    args.output.write_text(json.dumps({"schema": 1, "programs": results,
        "dynamic_system_dependency": "d3dcompiler_47.dll", "real_clean_machine": False}, indent=2) + "\n", encoding="utf-8")
    print("Verified x64 imports and app-local CRT for", len(results), "native files.")


if __name__ == "__main__":
    main()
