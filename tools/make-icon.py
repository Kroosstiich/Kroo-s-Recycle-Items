"""Write data/Interface/kroosrecycleitems/icons.swf: the RECYCLING section icon (frame label "recycle").

Same layout as CraftingCategories' own icons.swf: a 128 x 128 px stage, one labelled frame per icon, a white
shape inside a sprite. The icon is traced from tools/recycle-source.png (the recycling symbol chosen by
Kevin). Pass --preview to also write icon-preview.png next to this script for checking the drawing.
"""
import math
import struct
import sys
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / "data" / "Interface" / "kroosrecycleitems" / "icons.swf"
SOURCE = Path(__file__).with_name("recycle-source.png")  # recycling symbol, black outlines on white
SIZE = 2560  # stage size in twips (128 px)


class Bits:
    def __init__(self):
        self.bits = []

    def ub(self, n, v):
        self.bits += [(v >> (n - 1 - i)) & 1 for i in range(n)]

    def sb(self, n, v):
        self.ub(n, v & ((1 << n) - 1))

    def bytes(self):
        bits = self.bits + [0] * (-len(self.bits) % 8)
        return bytes(int("".join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def nbits(*values):
    return max(max(abs(v) for v in values).bit_length() + 1, 2)


def rect(xmin, xmax, ymin, ymax):
    b = Bits()
    n = nbits(xmin, xmax, ymin, ymax)
    b.ub(5, n)
    for v in (xmin, xmax, ymin, ymax):
        b.sb(n, v)
    return b.bytes()


def tag(code, data):
    if len(data) < 0x3F:
        return struct.pack("<H", code << 6 | len(data)) + data
    return struct.pack("<HI", code << 6 | 0x3F, len(data)) + data


def trace(png):
    """The white faces of the outlined recycling symbol in png (black outlines on white), as clockwise polygons
    in twips. The outlines become the thin dark gaps between the faces, which keeps the folds of the arrows."""
    import cv2
    import numpy as np

    gray = cv2.imread(str(png), cv2.IMREAD_GRAYSCALE)
    white = (gray >= 128).astype(np.uint8)
    count, labels = cv2.connectedComponents(white, connectivity=4)
    border = set(np.unique(np.concatenate([labels[0], labels[-1], labels[:, 0], labels[:, -1]])))
    faces = []
    for label in range(1, count):
        mask = (labels == label).astype(np.uint8)
        if label in border or mask.sum() < white.size * 0.003:
            continue
        mask = cv2.dilate(mask, np.ones((9, 9), np.uint8))  # narrow the gaps, without making faces overlap
        contour = max(cv2.findContours(mask, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_NONE)[0], key=cv2.contourArea)
        faces.append(cv2.approxPolyDP(contour, 1.0, True)[:, 0, :].tolist())
    xs = [x for f in faces for x, _ in f]
    ys = [y for f in faces for _, y in f]
    scale = 2400 / max(max(xs) - min(xs), max(ys) - min(ys))
    ox = (SIZE - (max(xs) - min(xs)) * scale) / 2 - min(xs) * scale
    oy = (SIZE - (max(ys) - min(ys)) * scale) / 2 - min(ys) * scale
    polygons = []
    for face in faces:
        poly = []
        for x, y in face:
            point = (round(ox + x * scale), round(oy + y * scale))
            if not poly or point != poly[-1]:
                poly.append(point)
        if shoelace(poly) < 0:
            poly.reverse()
        polygons.append(poly)
    return polygons


def shoelace(poly):
    return sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1]
               for i in range(len(poly))) / 2


def define_shape(shape_id, polygons):
    xs = [x for p in polygons for x, _ in p]
    ys = [y for p in polygons for _, y in p]
    data = struct.pack("<H", shape_id) + rect(min(xs), max(xs), min(ys), max(ys))
    data += bytes([1, 0x00, 0xFF, 0xFF, 0xFF])  # one solid white fill
    data += bytes([0, 0x10])  # no line style; 1 fill bit, 0 line bits
    b = Bits()
    for poly in polygons:
        # Fill style 1 on a clockwise outline, as in CraftingCategories' icons.swf.
        assert shoelace(poly) > 0
        x, y = poly[0]
        b.ub(1, 0)
        b.ub(5, 0b00101)  # fill style 1 + move to
        n = nbits(x, y)
        b.ub(5, n)
        b.sb(n, x)
        b.sb(n, y)
        b.ub(1, 1)  # fill style 1
        for nx, ny in poly[1:] + poly[:1]:
            dx, dy = nx - x, ny - y
            n = nbits(dx, dy)
            b.ub(2, 0b11)  # edge, straight
            b.ub(4, n - 2)
            b.ub(1, 1)  # general line
            b.sb(n, dx)
            b.sb(n, dy)
            x, y = nx, ny
    b.ub(6, 0)  # end of shape
    return tag(2, data + b.bytes())


def place(char_id):
    return tag(26, bytes([0x06]) + struct.pack("<HH", 1, char_id) + b"\x00")  # depth 1, identity matrix


def main():
    polygons = trace(SOURCE)
    sprite = struct.pack("<HH", 2, 1) + place(1) + tag(1, b"") + tag(0, b"")
    body = (rect(0, SIZE, 0, SIZE) + struct.pack("<HH", 24 << 8, 1)
            + tag(69, b"\x00\x00\x00\x00") + tag(9, b"\x00\x00\x00") + tag(12, b"\x07\x00")
            + tag(43, b"recycle\x00") + define_shape(1, polygons) + tag(39, sprite) + place(2)
            + tag(1, b"") + tag(0, b""))
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(b"CWS" + bytes([10]) + struct.pack("<I", 8 + len(body)) + zlib.compress(body))
    print(OUT, OUT.stat().st_size, "bytes")

    if "--preview" in sys.argv:
        from PIL import Image, ImageDraw
        img = Image.new("RGB", (256, 256), (30, 30, 30))
        draw = ImageDraw.Draw(img)
        for poly in polygons:
            draw.polygon([(x / 10, y / 10) for x, y in poly], fill=(255, 255, 255))
        img.save(Path(__file__).with_name("icon-preview.png"))


if __name__ == "__main__":
    main()
