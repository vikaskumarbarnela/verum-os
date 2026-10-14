"""Convert a QEMU screendump (binary PPM) to PNG with only the standard library."""
import struct, sys, zlib
src, dst = sys.argv[1], sys.argv[2]
d = open(src, "rb").read()
parts = d.split(b"\n", 3); w, h = map(int, parts[1].split()); px = parts[3]
raw = b"".join(b"\x00" + px[y * w * 3:(y + 1) * w * 3] for y in range(h))
def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
open(dst, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))
print(f"{dst}: {w}x{h}")
