"""Rasterize the TalkText icon to Windows ICO; development only, requires Pillow.
Orchard brand family: dark rounded tile, Orchard leaf green, white stem.
Mic geometry: https://github.com/lucide-icons/lucide/blob/main/icons/mic.svg (ISC).
"""
from pathlib import Path
from PIL import Image, ImageDraw

TILE = (17, 22, 19, 255)     # Orchard logo tile
LEAF = (152, 228, 80, 255)   # Orchard logo leaves
STEM = (252, 251, 251, 255)  # Orchard logo trunk
SUPERSAMPLE = 8

root = Path(__file__).resolve().parent.parent


def render(size: int) -> Image.Image:
    big = size * SUPERSAMPLE
    image = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    draw = ImageDraw.Draw(image)
    draw.rounded_rectangle((0, 0, big - 1, big - 1), radius=round(big * 0.22), fill=TILE)
    # Lucide's 24-unit mic, centred in the tile's inner 64%.
    unit = big * 0.64 / 24
    origin = big * 0.18

    def pt(x: float, y: float) -> tuple[float, float]:
        return origin + x * unit, origin + y * unit

    stroke = max(SUPERSAMPLE, round(2.2 * unit))
    draw.rounded_rectangle((*pt(9, 2), *pt(15, 15)), radius=round(3 * unit), fill=LEAF)
    draw.arc((*pt(5, 5), *pt(19, 19)), 0, 180, fill=STEM, width=stroke)
    for x in (5, 19):
        draw.line((*pt(x, 10), *pt(x, 12)), fill=STEM, width=stroke)
    draw.line((*pt(12, 19), *pt(12, 22)), fill=STEM, width=stroke)
    draw.line((*pt(8, 22), *pt(16, 22)), fill=STEM, width=stroke)
    for x, y in ((5, 10), (19, 10), (8, 22), (16, 22)):
        cx, cy = pt(x, y)
        r = stroke / 2
        draw.ellipse((cx - r, cy - r, cx + r, cy + r), fill=STEM)
    return image.resize((size, size), Image.LANCZOS)


images = [render(size) for size in (16, 24, 32, 48, 64, 128, 256)]
images[-1].save(root / 'assets/talktext.ico', append_images=images[:-1], sizes=[im.size for im in images])
