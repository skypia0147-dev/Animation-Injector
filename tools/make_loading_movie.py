"""Build the transparent, script-free host for the native Scaleform overlay.

The C++ menu creates the shapes/text through GFxValue. No Flash compiler,
external font, executable bytecode or third-party runtime is embedded here.
Format: Adobe SWF File Format Specification v19, header/RECT/ShowFrame/End.
https://open-flash.github.io/mirrors/swf-spec-19.pdf
"""
from pathlib import Path
import struct


def movie() -> bytes:
    values = (0, 1280 * 20, 0, 720 * 20)
    width = max(values).bit_length() + 1
    bits = f"{width:05b}" + "".join(f"{v:0{width}b}" for v in values)
    bits += "0" * (-len(bits) % 8)
    rect = int(bits, 2).to_bytes(len(bits) // 8, "big")
    tags = struct.pack("<HIHH", (69 << 6) | 4, 0, 1 << 6, 0)
    body = rect + struct.pack("<HH", 30 << 8, 1) + tags
    return b"FWS\x08" + struct.pack("<I", len(body) + 8) + body


if __name__ == "__main__":
    path = Path(__file__).resolve().parents[1] / "assets/Interface/AnimationInjectorLoading.swf"
    path.parent.mkdir(parents=True, exist_ok=True)
    data = movie()
    path.write_bytes(data)
    print(f"{path}: {len(data)} bytes (no ActionScript, no network)")
