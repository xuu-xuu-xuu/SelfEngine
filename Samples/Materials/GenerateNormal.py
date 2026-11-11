"""生成确定性的切线空间 +Y 法线测试数据；无需第三方 Python 包。"""
import math
import pathlib
import struct
import zlib

size = 64
rows = bytearray()
for y in range(size):
    rows.append(0)
    for x in range(size):
        nx = 0.55 * math.cos(2 * math.pi * (x + 0.5) / 16)
        ny = 0.55 * math.sin(2 * math.pi * (y + 0.5) / 16)
        nz = math.sqrt(1 - nx * nx - ny * ny)
        rows.extend(round((value * 0.5 + 0.5) * 255) for value in (nx, ny, nz))
        rows.append(255)

def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))

target = pathlib.Path(__file__).resolve().parents[2] / "Resources/Samples/Materials/normal.png"
target.parent.mkdir(parents=True, exist_ok=True)
target.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
                   + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
