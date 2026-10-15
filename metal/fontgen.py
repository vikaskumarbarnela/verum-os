"""Generate kernel/font.h: an 8x16 ASCII bitmap font and a pre-shaped Devanagari word atlas.

Devanagari needs real text shaping (conjuncts, vowel signs that move before or
below their consonant). A 100 KiB kernel should not carry a shaping engine, so
the shaping is done here, at build time, by Windows GDI with the Nirmala UI
font: every Sanskrit word the kernel can show (the whole संस्कृतलिपि vocabulary
plus every Devanagari word in the embedded cells and a word list) is rendered
once into a 1-bit bitmap and embedded. At runtime the console blits words.

    python fontgen.py            -> kernel/font.h  (+ build/font-preview.png to eyeball)
"""

from __future__ import annotations

import ctypes
import os
import re
import struct
import sys
import zlib
from ctypes import wintypes as w

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "kernel", "font.h")
CELL_H = 20                     # line height in pixels: room for matras above and below
ASCII_W, ASCII_H = 8, 16
MAX_WORD_W = 160

gdi = ctypes.windll.gdi32
user = ctypes.windll.user32

# 64-bit handles must be declared, or ctypes truncates them to int
gdi.CreateCompatibleDC.restype = w.HDC
gdi.CreateCompatibleDC.argtypes = [w.HDC]
gdi.CreateDIBSection.restype = w.HBITMAP
gdi.CreateDIBSection.argtypes = [w.HDC, ctypes.c_void_p, w.UINT, ctypes.POINTER(ctypes.c_void_p), w.HANDLE, w.DWORD]
gdi.SelectObject.restype = w.HGDIOBJ
gdi.SelectObject.argtypes = [w.HDC, w.HGDIOBJ]
gdi.CreateFontW.restype = w.HFONT
gdi.CreateFontW.argtypes = [ctypes.c_int] * 5 + [w.DWORD] * 8 + [w.LPCWSTR]
gdi.SetBkColor.argtypes = [w.HDC, w.COLORREF]
gdi.SetTextColor.argtypes = [w.HDC, w.COLORREF]
gdi.ExtTextOutW.argtypes = [w.HDC, ctypes.c_int, ctypes.c_int, w.UINT, ctypes.c_void_p, w.LPCWSTR, w.UINT, ctypes.c_void_p]
gdi.GetTextExtentPoint32W.argtypes = [w.HDC, w.LPCWSTR, ctypes.c_int, ctypes.c_void_p]


class BITMAPINFOHEADER(ctypes.Structure):
    _fields_ = [("biSize", w.DWORD), ("biWidth", w.LONG), ("biHeight", w.LONG), ("biPlanes", w.WORD),
                ("biBitCount", w.WORD), ("biCompression", w.DWORD), ("biSizeImage", w.DWORD),
                ("biXPelsPerMeter", w.LONG), ("biYPelsPerMeter", w.LONG), ("biClrUsed", w.DWORD), ("biClrImportant", w.DWORD)]


class BITMAPINFO(ctypes.Structure):
    _fields_ = [("bmiHeader", BITMAPINFOHEADER), ("bmiColors", w.DWORD * 3)]


class Canvas:
    """A top-down 32-bit DIB we can draw text into and read pixels back from."""

    def __init__(self, width: int, height: int) -> None:
        self.w, self.h = width, height
        self.hdc = gdi.CreateCompatibleDC(None)
        bmi = BITMAPINFO()
        bmi.bmiHeader.biSize = ctypes.sizeof(BITMAPINFOHEADER)
        bmi.bmiHeader.biWidth, bmi.bmiHeader.biHeight = width, -height
        bmi.bmiHeader.biPlanes, bmi.bmiHeader.biBitCount = 1, 32
        self.bits = ctypes.c_void_p()
        gdi.CreateDIBSection.restype = w.HBITMAP
        self.hbm = gdi.CreateDIBSection(self.hdc, ctypes.byref(bmi), 0, ctypes.byref(self.bits), None, 0)
        gdi.SelectObject(self.hdc, self.hbm)
        gdi.SetBkColor(self.hdc, 0x000000)
        gdi.SetTextColor(self.hdc, 0xFFFFFF)

    def font(self, face: str, height: int, bold: bool = False) -> None:
        NONANTIALIASED_QUALITY = 3
        gdi.CreateFontW.restype = w.HFONT
        hf = gdi.CreateFontW(-height, 0, 0, 0, 700 if bold else 400, 0, 0, 0, 1, 0, 0, NONANTIALIASED_QUALITY, 0, face)
        gdi.SelectObject(self.hdc, hf)

    def clear(self) -> None:
        ctypes.memset(self.bits, 0, self.w * self.h * 4)

    def text(self, x: int, y: int, s: str) -> int:
        ETO_OPAQUE = 2
        rc = w.RECT(0, 0, self.w, self.h)
        gdi.ExtTextOutW(self.hdc, x, y, ETO_OPAQUE, ctypes.byref(rc), s, len(s), None)
        size = w.SIZE()
        gdi.GetTextExtentPoint32W(self.hdc, s, len(s), ctypes.byref(size))
        return size.cx

    def pixels(self):
        buf = (ctypes.c_uint32 * (self.w * self.h)).from_address(self.bits.value)
        return [[(buf[y * self.w + x] & 0xFFFFFF) != 0 for x in range(self.w)] for y in range(self.h)]


def render_ascii(c: Canvas):
    """95 printable ASCII glyphs as 16 rows of 8 bits each."""
    c.font("Consolas", 15)
    glyphs = []
    for code in range(32, 127):
        c.clear()
        c.text(0, 0, chr(code))
        px = c.pixels()
        rows = []
        for y in range(ASCII_H):
            bits = 0
            for x in range(ASCII_W):
                if px[y][x]:
                    bits |= 0x80 >> x
            rows.append(bits)
        glyphs.append(rows)
    return glyphs


def render_word(c: Canvas, word: str):
    """A shaped Devanagari word: width, then CELL_H rows of ceil(width/8) bytes."""
    c.font("Nirmala UI", 15)
    c.clear()
    width = min(c.text(0, 0, word), MAX_WORD_W)
    px = c.pixels()
    # trim to the ink's right edge but keep at least the measured width
    rows = []
    nbytes = (width + 7) // 8
    for y in range(CELL_H):
        line = []
        for b in range(nbytes):
            bits = 0
            for i in range(8):
                x = b * 8 + i
                if x < width and px[y][x]:
                    bits |= 0x80 >> i
            line.append(bits)
        rows.append(line)
    return width, rows


DEVANAGARI = re.compile(r"[ऀ-ॿ]+")


def collect_words() -> list[str]:
    sys.path.insert(0, os.path.dirname(HERE))
    from verum import sanskrit
    words = set(sanskrit.SYMBOLS) | set(sanskrit.OPS)
    for path in (os.path.join(HERE, "kernel", "cells.c"), os.path.join(HERE, "words.txt"),
                 os.path.join(os.path.dirname(HERE), "cells", "ganana.sv")):
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                words |= set(DEVANAGARI.findall(f.read()))
    words.discard("")
    return sorted(words)


def write_png(path: str, pixels, width: int, height: int) -> None:
    raw = b"".join(b"\x00" + bytes(255 if pixels[y][x] else 0 for x in range(width)) for y in range(height))
    def chunk(t, b): return struct.pack(">I", len(b)) + t + b + struct.pack(">I", zlib.crc32(t + b) & 0xffffffff)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


def main() -> None:
    c = Canvas(MAX_WORD_W, CELL_H)
    ascii_glyphs = render_ascii(c)
    words = collect_words()
    atlas = [(wd, *render_word(c, wd)) for wd in words]

    lines = ["/* generated by fontgen.py - do not edit */", "#ifndef VERUM_FONT_H", "#define VERUM_FONT_H",
             f"#define FONT_ASCII_W {ASCII_W}", f"#define FONT_ASCII_H {ASCII_H}", f"#define FONT_CELL_H {CELL_H}",
             f"#define FONT_WORDS {len(atlas)}", f"#define FONT_MAX_WORD_W {MAX_WORD_W}",
             "static const unsigned char FONT_ASCII[95][16] = {"]
    for rows in ascii_glyphs:
        lines.append("    {" + ",".join(f"0x{b:02x}" for b in rows) + "},")
    lines.append("};")
    lines.append("typedef struct { const char *utf8; unsigned short width; const unsigned char *bits; } FontWord;")
    for i, (wd, width, rows) in enumerate(atlas):
        flat = [b for row in rows for b in row]
        lines.append(f"static const unsigned char FONT_W{i}[] = {{" + ",".join(f"0x{b:02x}" for b in flat) + "};")
    lines.append("static const FontWord FONT_WORD[] = {")
    for i, (wd, width, rows) in enumerate(atlas):
        esc = "".join(f"\\x{b:02x}" for b in wd.encode("utf-8"))
        lines.append(f'    {{"{esc}", {width}, FONT_W{i}}},  /* {wd} */')
    lines.append("};")
    lines.append("#endif")
    with open(OUT, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    # preview: ASCII sample + the atlas
    pw, ph = 640, 16 + CELL_H * ((len(atlas) + 3) // 4)
    canvas = [[False] * pw for _ in range(ph)]
    for i, ch in enumerate("VERUM OS 0123456789 verum> abc"):
        g = ascii_glyphs[ord(ch) - 32]
        for y in range(16):
            for x in range(8):
                if g[y] & (0x80 >> x) and i * 8 + x < pw:
                    canvas[y][i * 8 + x] = True
    for n, (wd, width, rows) in enumerate(atlas):
        ox, oy = (n % 4) * 160, 16 + (n // 4) * CELL_H
        for y in range(CELL_H):
            for b, byte in enumerate(rows[y]):
                for i in range(8):
                    if byte & (0x80 >> i) and ox + b * 8 + i < pw and oy + y < ph:
                        canvas[oy + y][ox + b * 8 + i] = True
    os.makedirs(os.path.join(HERE, "build"), exist_ok=True)
    write_png(os.path.join(HERE, "build", "font-preview.png"), canvas, pw, ph)
    size = os.path.getsize(OUT)
    print(f"font.h: 95 ASCII glyphs, {len(atlas)} Devanagari words, {size // 1024} KiB; preview in build/font-preview.png")


if __name__ == "__main__":
    main()
