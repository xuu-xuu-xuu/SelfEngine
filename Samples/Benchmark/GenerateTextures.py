"""生成四套确定性压力测试棋盘纹理，无外部图片与依赖。"""
from pathlib import Path
import struct
import zlib

target = Path(__file__).resolve().parents[2] / "Resources/Samples/Benchmark"
target.mkdir(parents=True, exist_ok=True)
colors = ((230, 180, 75), (75, 210, 150), (80, 130, 240), (210, 80, 175))
def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
for index, color in enumerate(colors):
    rows = bytearray()
    for y in range(16):
        rows.append(0)
        for x in range(16):
            weight = 1 if (x // 4 + y // 4) % 2 == 0 else 0.35
            rows.extend(round(channel * weight) for channel in color)
            rows.append(255)
    (target / f"texture-{index}.png").write_bytes(b"\x89PNG\r\n\x1a\n"
        + chunk(b"IHDR", struct.pack(">IIBBBBB", 16, 16, 8, 6, 0, 0, 0))
        + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
