#!/usr/bin/env python3
"""Draw the launcher icons.

The icon is the app's own step lane display: three rows of steps with one
column lit as the playhead. It has to survive being 16 pixels wide, so each
size is drawn on its own integer grid rather than scaled down from the largest.
"""
from PIL import Image, ImageDraw

BG = (16, 16, 20, 255)
OFF = (38, 38, 46, 255)
ON = (47, 224, 168, 255)
HEAD = (255, 194, 74, 255)

# Kick, snare and hats. The hats are drawn as short ticks rather than full
# blocks so the three rows read as a rhythm instead of a checkerboard.
ROWS = [
    ([1, 0, 0, 1], "block"),
    ([0, 0, 1, 0], "block"),
    ([1, 1, 1, 1], "tick"),
]
PLAYHEAD = 2

# size: (x0, pitch, w, y0, ypitch, h, corner radius)
LAYOUT = {
    16: (1, 4, 3, 1, 5, 3, 3),
    32: (2, 8, 6, 3, 10, 6, 6),
    64: (5, 15, 12, 7, 20, 12, 13),
}


def draw(size):
    x0, px, w, y0, py, h, radius = LAYOUT[size]
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([0, 0, size - 1, size - 1], radius=radius, fill=BG)

    for r, (row, style) in enumerate(ROWS):
        for c, on in enumerate(row):
            x, y = x0 + c * px, y0 + r * py
            colour = HEAD if c == PLAYHEAD else ON
            if on and style == "block":
                d.rectangle([x, y, x + w - 1, y + h - 1], fill=colour)
            elif on:
                tick = max(1, h // 2)
                d.rectangle([x, y + h - tick, x + w - 1, y + h - 1], fill=colour)
            else:
                # An unlit step is a faint bar, the way the app draws them
                thin = max(1, h // 4)
                mid = y + (h - thin) // 2
                d.rectangle([x, mid, x + w - 1, mid + thin - 1], fill=OFF)
    return img


for size in (16, 32, 64):
    draw(size).save(f"icons/icon{size}.png")
    print(f"icons/icon{size}.png")
