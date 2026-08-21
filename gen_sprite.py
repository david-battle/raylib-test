import struct, zlib

W = H = 64
px = [[(0, 0, 0, 0)] * W for _ in range(H)]

BODY = (124, 77, 210, 255)
OUTLINE = (58, 32, 110, 255)
HILITE = (168, 130, 240, 255)
FOOT = (95, 55, 170, 255)
MOUTH = (40, 18, 75, 255)
FANG = (245, 245, 255, 255)
BALL = (255, 92, 56, 255)
WHITE = (255, 255, 255, 255)

def put(x, y, c):
    if 0 <= x < W and 0 <= y < H:
        px[y][x] = c

def fill_ellipse(cx, cy, rx, ry, c):
    for y in range(H):
        for x in range(W):
            dx, dy = (x - cx) / rx, (y - cy) / ry
            v = dx * dx + dy * dy
            if v <= 1.0:
                put(x, y, c)

# Body with outline ring
for y in range(H):
    for x in range(W):
        dx, dy = (x - 32) / 24.0, (y - 39) / 23.0
        v = dx * dx + dy * dy
        if v <= 1.0:
            put(x, y, OUTLINE if v > 0.86 else BODY)

# Gloss highlight upper-left
fill_ellipse(25, 28, 11, 8, HILITE)

# Feet
fill_ellipse(23, 62, 5, 3, FOOT)
fill_ellipse(41, 62, 5, 3, FOOT)

# Mouth is NOT drawn here - main.c draws it per animation frame
# (closed grin vs open chomp).

# Antenna
for y in range(9, 16):
    put(31, y, OUTLINE); put(32, y, OUTLINE)
fill_ellipse(31, 6, 3, 3, BALL)
put(30, 5, WHITE)

# Eyes (same geometry the pupil/blink code expects: centers ~26 and ~38, cy ~44)
fill_ellipse(26, 44, 4.2, 5.2, WHITE)
fill_ellipse(38, 44, 4.2, 5.2, WHITE)

raw = bytearray()
for y in range(H):
    raw.append(0)
    for x in range(W):
        raw.extend(px[y][x])

def chunk(typ, data):
    c = struct.pack('>I', len(data)) + typ + data
    return c + struct.pack('>I', zlib.crc32(typ + data) & 0xFFFFFFFF)

png = b'\x89PNG\r\n\x1a\n'
png += chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 6, 0, 0, 0))
png += chunk(b'IDAT', zlib.compress(bytes(raw)))
png += chunk(b'IEND', b'')

open('resources/sprite.png', 'wb').write(png)
print('wrote resources/sprite.png')
