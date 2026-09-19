"""Rasterize Lucide mic geometry to Windows ICO; development only, requires Pillow.
Source: https://github.com/lucide-icons/lucide/blob/main/icons/mic.svg (ISC).
"""
from pathlib import Path
from PIL import Image, ImageDraw
root = Path(__file__).resolve().parent.parent
(root / 'assets').mkdir(exist_ok=True)
images = []
for size in (16, 24, 32, 48, 64, 128, 256):
    image = Image.new('RGBA', (size, size), (0, 0, 0, 255))
    draw = ImageDraw.Draw(image)
    scale = size / 28
    def box(values):
        return tuple(round((v + 2) * scale) for v in values)
    stroke = max(1, round(2 * scale))
    draw.rounded_rectangle(box((9, 2, 15, 15)), radius=round(3*scale), outline='white', width=stroke)
    draw.arc(box((5, 5, 19, 19)), 0, 180, fill='white', width=stroke)
    draw.line(box((5, 10, 5, 12)), fill='white', width=stroke)
    draw.line(box((19, 10, 19, 12)), fill='white', width=stroke)
    draw.line(box((12, 19, 12, 22)), fill='white', width=stroke)
    images.append(image)
images[-1].save(root/'assets/mic.ico', append_images=images[:-1], sizes=[im.size for im in images])
