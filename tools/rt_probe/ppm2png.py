import sys, zlib, struct
def ppm2png(src, dst, scale=2):
    data = open(src, 'rb').read()
    parts = data.split(b'\n', 1)
    _, w, h, _ = parts[0].split()
    w, h = int(w), int(h)
    px = parts[1]
    rows = []
    for y in range(h):
        row = px[y * w * 3:(y + 1) * w * 3]
        big = b''.join(row[x * 3:x * 3 + 3] * scale for x in range(w))
        for _ in range(scale): rows.append(b'\x00' + big)
    raw = b''.join(rows)
    def chunk(t, d): return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 2, 0, 0, 0)) + \
          chunk(b'IDAT', zlib.compress(raw, 9)) + chunk(b'IEND', b'')
    open(dst, 'wb').write(png)
ppm2png(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 2)
