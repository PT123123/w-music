"""Generates the placeholder MSIX assets (solid-colour PNGs) for w-music.

Run:  python tools/gen_assets.py
"""
import os
import struct
import zlib

ASSETS = [
    ("StoreLogo.png", 50, 50),
    ("Square44x44Logo.png", 44, 44),
    ("Square150x150Logo.png", 150, 150),
    ("Wide310x150Logo.png", 310, 150),
    ("SplashScreen.png", 620, 300),
    ("LockScreenLogo.png", 24, 24),
]


def mix(a, b, t):
    return tuple(int(a[i] + (b[i] - a[i]) * t) for i in range(3))


def write_png(path, w, h):
    top = (0x1F, 0x2B, 0x38)
    bottom = (0x31, 0xC2, 0x7C)  # QQ Music-ish green
    rows = bytearray()
    for y in range(h):
        rows.append(0)  # filter type: none
        colour = mix(top, bottom, y / max(1, h - 1))
        rows.extend(bytes(colour + (255,)) * w)

    def chunk(tag, data):
        return (
            struct.pack(">I", len(data))
            + tag
            + data
            + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
        )

    header = b"\x89PNG\r\n\x1a\n"
    ihdr = struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0)
    blob = (
        header
        + chunk(b"IHDR", ihdr)
        + chunk(b"IDAT", zlib.compress(bytes(rows), 9))
        + chunk(b"IEND", b"")
    )
    with open(path, "wb") as f:
        f.write(blob)


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    out = os.path.join(here, "..", "src", "w-music", "Assets")
    os.makedirs(out, exist_ok=True)
    for name, w, h in ASSETS:
        target = os.path.join(out, name)
        write_png(target, w, h)
        print("wrote", os.path.normpath(target), f"{w}x{h}")


if __name__ == "__main__":
    main()
