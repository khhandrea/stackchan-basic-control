"""Generate the 16 px bitmap font used for modal text (main/assets/font16.fnt).

Source: Noto Sans Mono CJK KR (SIL Open Font License 1.1), from the system noto-cjk package.
Run: uv run --with pillow python tools/gen_font.py [--preview out.png]

Layout (1 bpp, rows top to bottom, MSB = leftmost pixel):
  ASCII 0x20-0x7E          95 glyphs,  8x16, 16 bytes each
  Hangul jamo 0x3131-0x318E 94 glyphs, 16x16, 32 bytes each
  Hangul syllables 0xAC00-0xD7A3 11172 glyphs, 16x16, 32 bytes each
"""

import argparse
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT = "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"
FACE_INDEX = 6  # Noto Sans Mono CJK KR
SIZE = 16
THRESHOLD = 110
RANGES = [(0x20, 0x7E, 8), (0x3131, 0x318E, 16), (0xAC00, 0xD7A3, 16)]
OUT = Path(__file__).resolve().parent.parent / "main" / "assets" / "font16.fnt"


def render(font: ImageFont.FreeTypeFont, ch: str, width: int) -> list[list[bool]]:
    img = Image.new("L", (width, SIZE), 0)
    # Baseline at 0.88 em puts the em square (ascender 0.88, descender 0.12) on rows 0-15.
    ImageDraw.Draw(img).text((0, round(SIZE * 0.88)), ch, fill=255, font=font, anchor="ls")
    return [[img.getpixel((x, y)) >= THRESHOLD for x in range(width)] for y in range(SIZE)]


def pack(rows: list[list[bool]]) -> bytes:
    out = bytearray()
    for row in rows:
        bits = 0
        for x, on in enumerate(row):
            bits |= on << (len(row) - 1 - x)
        out += bits.to_bytes(len(row) // 8, "big")
    return bytes(out)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--preview", type=Path, help="write a PNG sample instead of the font file")
    args = parser.parse_args()
    font = ImageFont.truetype(FONT, SIZE, index=FACE_INDEX)

    if args.preview:
        sample = "Hello, StackChan! 0123 안녕하세요 ㅋㅋ"
        x = 0
        img = Image.new("1", (8 * 2 * len(sample), SIZE), 0)
        for ch in sample:
            w = 8 if ord(ch) < 0x80 else 16
            rows = render(font, ch, w)
            for yy, row in enumerate(rows):
                for xx, on in enumerate(row):
                    if on:
                        img.putpixel((x + xx, yy), 1)
            x += w
        img.crop((0, 0, x, SIZE)).resize((x * 4, SIZE * 4), Image.NEAREST).save(args.preview)
        return

    data = bytearray()
    for first, last, width in RANGES:
        for code in range(first, last + 1):
            data += pack(render(font, chr(code), width))
    OUT.write_bytes(data)
    print(f"{OUT}: {len(data)} bytes")


if __name__ == "__main__":
    main()
